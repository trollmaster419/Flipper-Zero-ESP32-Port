#include "elf_file.h"
#include "elf_file_i.h"

#include <storage/storage.h>
#include "elf_api_interface.h"
#include "../api_hashtable/api_hashtable.h"

#include <furi.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#endif

#include <stdlib.h>
#include <string.h>

#define TAG "Elf"

#define ELF_NAME_BUFFER_LEN        32
#define SECTION_OFFSET(e, n)       ((e)->section_table + (n) * sizeof(Elf32_Shdr))
#define IS_FLAGS_SET(v, m)         (((v) & (m)) == (m))
#define RESOLVER_THREAD_YIELD_STEP 30

// #define ELF_DEBUG_LOG 1

#ifndef ELF_DEBUG_LOG
#undef FURI_LOG_D
#define FURI_LOG_D(...)
#endif

#ifdef ESP_PLATFORM
/* PSRAM-first allocation: keeps internal DRAM free for FreeRTOS objects,
 * I2C/SPI command links, etc. Falls back to default heap if PSRAM full. */
static inline void* elf_psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p) p = malloc(size);
    return p;
}
#else
#define elf_psram_malloc(size) malloc(size)
#endif

#define ELF_INVALID_ADDRESS 0xFFFFFFFF

/* ESP32-S3: Convert PSRAM data bus address to instruction bus address.
 * Same physical memory, different virtual address:
 *   Data bus:        0x3C000000 - 0x3DFFFFFF
 *   Instruction bus: 0x42000000 - 0x43FFFFFF
 * Offset: 0x06000000 */
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#define PSRAM_DATA_TO_INST(addr) \
    ( \
      /* ESP32-S3 mapping: 0x3C -> 0x42 (offset 0x06000000) */ \
      ((addr) >= 0x3C000000 && (addr) < 0x3E000000) ? ((addr) + 0x06000000) : \
      /* ESP32 PSRAM: 0x3F80 -> 0x4080 (offset 0x01000000) */ \
      ((addr) >= 0x3F800000 && (addr) < 0x3FC00000) ? ((addr) + 0x01000000) : \
      /* ESP32 Internal: use IDF helper if available, otherwise fallback */ \
      esp_ptr_in_diram_dram((void*)(addr)) ? (uint32_t)esp_ptr_diram_dram_to_iram((void*)(addr)) : \
      (addr) \
    )
#define PSRAM_INST_TO_DATA(addr) \
    ( \
      /* ESP32-S3 mapping: 0x42 -> 0x3C (offset -0x06000000) */ \
      ((addr) >= 0x42000000 && (addr) < 0x44000000) ? ((addr) - 0x06000000) : \
      /* ESP32 PSRAM: 0x4080 -> 0x3F80 (offset -0x01000000) */ \
      ((addr) >= 0x40800000 && (addr) < 0x40C00000) ? ((addr) - 0x01000000) : \
      /* ESP32 Internal: use IDF helper for IRAM -> DRAM */ \
      esp_ptr_in_diram_iram((void*)(addr)) ? (uint32_t)esp_ptr_diram_iram_to_dram((void*)(addr)) : \
      (addr) \
    )
#else
#define PSRAM_DATA_TO_INST(addr) (addr)
#define PSRAM_INST_TO_DATA(addr) (addr)
#endif

/**************************************************************************************************/
/************************************ FAP executable code pool ***********************************/
/**************************************************************************************************/
/* Classic ESP32 can only execute dynamically-loaded code from internal IRAM, and at runtime the
 * exec-capable heap is fragmented down to a ~31 KiB hole (see fap-loader-iram-exec). To run larger
 * FAPs we reserve ONE big contiguous exec block at early boot (before BLE/services fragment
 * D/IRAM) and hand out FAP .text from it with a trivial refcount bump allocator. No in-band
 * metadata is written into the block (IRAM is not byte-writable and D/IRAM is word-inverted), so
 * the block stays pure code storage; the loader still writes through the DRAM mirror as usual.
 * FAPs load and free their sections as a unit, so when the alloc count returns to zero the bump
 * offset resets and the whole pool is reusable for the next FAP. */
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32)
static uint8_t* fap_exec_pool_base = NULL; /* IRAM (instruction-bus) view */
static size_t fap_exec_pool_size = 0;
static size_t fap_exec_pool_offset = 0;
static int fap_exec_pool_count = 0;

/* Leave this much exec-capable RAM for the rest of the firmware; never grab it all. */
#define FAP_EXEC_POOL_HEADROOM (40 * 1024)
#define FAP_EXEC_POOL_MIN (8 * 1024)

static void* fap_exec_pool_alloc_stack(size_t size);
static void fap_exec_pool_free_stack(void* p);

void fap_exec_pool_init(size_t requested) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
    size_t free_total = heap_caps_get_free_size(MALLOC_CAP_EXEC);
    FURI_LOG_I(
        TAG,
        "fap exec pool: at boot free=%u largest=%u, requesting %u",
        (unsigned)free_total,
        (unsigned)largest,
        (unsigned)requested);

    size_t max_take = (largest > FAP_EXEC_POOL_HEADROOM) ? (largest - FAP_EXEC_POOL_HEADROOM) : 0;
    size_t size = (requested < max_take) ? requested : max_take;
    size &= ~0xFFFu; /* 4 KiB granularity */
    if(size < FAP_EXEC_POOL_MIN) {
        FURI_LOG_E(TAG, "fap exec pool: not enough contiguous exec RAM (max_take=%u)", (unsigned)max_take);
        return;
    }

    void* p = heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if(!p) {
        FURI_LOG_E(TAG, "fap exec pool: reserve of %u FAILED", (unsigned)size);
        return;
    }
    fap_exec_pool_base = p;
    fap_exec_pool_size = size;
    fap_exec_pool_offset = 0;
    fap_exec_pool_count = 0;
    FURI_LOG_I(TAG, "fap exec pool: reserved %u bytes @ %p", (unsigned)size, p);

    /* Let FAP thread stacks come from the pool's contiguous internal leftover instead of
       falling back to PSRAM (which DoubleExceptions on cache-disabled flash ops). */
    furi_thread_set_ext_stack_allocator(fap_exec_pool_alloc_stack, fap_exec_pool_free_stack);
}

void fap_exec_pool_deinit(void) {
    if(!fap_exec_pool_base) return;
    FURI_LOG_I(TAG, "fap exec pool: releasing %u bytes @ %p", (unsigned)fap_exec_pool_size, fap_exec_pool_base);
    heap_caps_free(fap_exec_pool_base);
    fap_exec_pool_base = NULL;
    fap_exec_pool_size = 0;
    fap_exec_pool_offset = 0;
    fap_exec_pool_count = 0;
    /* Remove the ext-stack allocator so new FAP threads fall back to the default allocator. */
    furi_thread_set_ext_stack_allocator(NULL, NULL);
}

static void* fap_exec_pool_alloc(size_t size, size_t align) {
    if(!fap_exec_pool_base) return NULL;
    if(align < 4) align = 4;
    size_t off = (fap_exec_pool_offset + (align - 1)) & ~(align - 1);
    if(off + size > fap_exec_pool_size) return NULL; /* doesn't fit; caller falls back to heap */
    void* p = fap_exec_pool_base + off;
    fap_exec_pool_offset = off + size;
    fap_exec_pool_count++;
    return p;
}

static bool fap_exec_pool_contains(const void* p) {
    return fap_exec_pool_base && (const uint8_t*)p >= fap_exec_pool_base &&
           (const uint8_t*)p < (fap_exec_pool_base + fap_exec_pool_size);
}

static void fap_exec_pool_free(const void* p) {
    if(!fap_exec_pool_contains(p)) return;
    if(--fap_exec_pool_count <= 0) {
        fap_exec_pool_count = 0;
        fap_exec_pool_offset = 0; /* pool drained -> reusable */
    }
}

/* Stack allocator handed to furi_thread: bump-allocates from the pool but returns the DATA-bus
   (DRAM mirror) address, since stacks are accessed via the data bus. On classic ESP32 the D/IRAM
   region is word-inverted (instruction and data buses run opposite directions), so the two ends
   of the reserved instruction-bus range swap on the data bus -- return the lower data address so
   the buffer is a normal ascending [lo, lo+size). The allocation shares the pool's refcount, so
   when the FAP's code AND stack are both freed the bump offset resets. */
static void* fap_exec_pool_alloc_stack(size_t size) {
    void* iram = fap_exec_pool_alloc(size, 16);
    if(!iram) return NULL;
    uintptr_t d0 = (uintptr_t)PSRAM_INST_TO_DATA((uintptr_t)iram);
    uintptr_t d1 = (uintptr_t)PSRAM_INST_TO_DATA((uintptr_t)iram + size - 4);
    return (void*)((d0 < d1) ? d0 : d1);
}

static void fap_exec_pool_free_stack(void* p) {
    (void)p; /* data-bus address; not in the instruction-bus pool range, so just drop the refcount */
    if(!fap_exec_pool_base) return;
    if(--fap_exec_pool_count <= 0) {
        fap_exec_pool_count = 0;
        fap_exec_pool_offset = 0;
    }
}
#else
void fap_exec_pool_init(size_t requested) {
    (void)requested;
}
void fap_exec_pool_deinit(void) {
}
#endif

/**************************************************************************************************/
/********************************************* Caches *********************************************/
/**************************************************************************************************/

static bool address_cache_get(AddressCache_t cache, int symEntry, Elf32_Addr* symAddr) {
    Elf32_Addr* addr = AddressCache_get(cache, symEntry);
    if(addr) {
        *symAddr = *addr;
        return true;
    } else {
        return false;
    }
}

static void address_cache_put(AddressCache_t cache, int symEntry, Elf32_Addr symAddr) {
    AddressCache_set_at(cache, symEntry, symAddr);
}

/**************************************************************************************************/
/********************************************** ELF ***********************************************/
/**************************************************************************************************/

#ifdef CONFIG_IDF_TARGET_ESP32
/* ESP32 IRAM (0x40...) ONLY supports word-aligned 32-bit access.
 * If a DRAM mirror is available via PSRAM_INST_TO_DATA, we use it for easy access.
 * Otherwise, we must perform 4-byte aligned Word Read-Modify-Write. */
/* IMPORTANT: the D/IRAM mirror is WORD-inverted (SOC_DIRAM_INVERTED): the conversion helper
 * esp_ptr_diram_iram_to_dram subtracts 4, so it is only correct for WORD-ALIGNED addresses. We
 * therefore always convert the word-aligned base and index bytes WITHIN the word (bytes keep their
 * order; only whole words are swapped between the I-bus and D-bus views). Converting a raw byte
 * address would land on the wrong byte and corrupt code -- which is exactly what crashed FAPs
 * loaded into the D/IRAM pool. For pure IRAM (identity mirror) and PSRAM (linear offset) this same
 * code path is also correct. */
static inline void* elf_iram_word_mirror(uintptr_t word_addr) {
    return (void*)(uintptr_t)PSRAM_INST_TO_DATA(word_addr);
}

static inline uint8_t elf_iram_read8(void* addr) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t word = a & ~0x3u;
    uint32_t off = a & 0x3u;
    uintptr_t dword = (uintptr_t)elf_iram_word_mirror(word);
    if(dword != word) return *((volatile uint8_t*)dword + off);
    /* pure IRAM: not byte-readable, extract from the 32-bit word */
    return (uint8_t)((*(volatile uint32_t*)word) >> (off * 8));
}

static inline void elf_iram_write8(void* addr, uint8_t val) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t word = a & ~0x3u;
    uint32_t off = a & 0x3u;
    uintptr_t dword = (uintptr_t)elf_iram_word_mirror(word);
    if(dword != word) {
        *((volatile uint8_t*)dword + off) = val;
        return;
    }
    /* pure IRAM: 32-bit read-modify-write */
    uint32_t w = *(volatile uint32_t*)word;
    w &= ~(0xFFu << (off * 8));
    w |= ((uint32_t)val << (off * 8));
    *(volatile uint32_t*)word = w;
}

static inline uint32_t elf_iram_read32(void* addr) {
    uintptr_t a = (uintptr_t)addr;
    if(a & 0x3u) {
        uint32_t v = 0;
        for(int i = 0; i < 4; i++) v |= ((uint32_t)elf_iram_read8((uint8_t*)addr + i)) << (i * 8);
        return v;
    }
    return *(volatile uint32_t*)elf_iram_word_mirror(a);
}

static inline void elf_iram_write32(void* addr, uint32_t val) {
    uintptr_t a = (uintptr_t)addr;
    if(a & 0x3u) {
        for(int i = 0; i < 4; i++) elf_iram_write8((uint8_t*)addr + i, (val >> (i * 8)) & 0xFF);
        return;
    }
    *(volatile uint32_t*)elf_iram_word_mirror(a) = val;
}
#else
#define elf_iram_read8(a)   (*(volatile uint8_t*)(a))
#define elf_iram_write8(a,v) (*(volatile uint8_t*)(a) = (v))
#define elf_iram_read32(a)  (*(volatile uint32_t*)(a))
#define elf_iram_write32(a,v) (*(volatile uint32_t*)(a) = (v))
#endif

static void elf_file_maybe_release_fd(ELFFile* elf) {
    if(elf->fd) {
        storage_file_free(elf->fd);
        elf->fd = NULL;
    }
}

static ELFSection* elf_file_get_section(ELFFile* elf, const char* name) {
    return ELFSectionDict_get(elf->sections, name);
}

static ELFSection* elf_file_get_or_put_section(ELFFile* elf, const char* name) {
    ELFSection* section_p = elf_file_get_section(elf, name);
    if(!section_p) {
        ELFSectionDict_set_at(
            elf->sections,
            strdup(name),
            (ELFSection){
                .data = NULL,
                .sec_idx = 0,
                .size = 0,
                .rel_count = 0,
                .rel_offset = 0,
            });
        section_p = elf_file_get_section(elf, name);
    }

    return section_p;
}

static bool elf_read_string_from_offset(ELFFile* elf, off_t offset, FuriString* name) {
    bool result = false;

    off_t old = storage_file_tell(elf->fd);

    do {
        if(!storage_file_seek(elf->fd, offset, true)) break;

        char buffer[ELF_NAME_BUFFER_LEN + 1];
        buffer[ELF_NAME_BUFFER_LEN] = 0;

        while(true) {
            size_t read = storage_file_read(elf->fd, buffer, ELF_NAME_BUFFER_LEN);
            furi_string_cat(name, buffer);
            if(strlen(buffer) < ELF_NAME_BUFFER_LEN) {
                result = true;
                break;
            }

            if(storage_file_get_error(elf->fd) != FSE_OK || read == 0) break;
        }

    } while(false);
    storage_file_seek(elf->fd, old, true);

    return result;
}

static bool elf_read_section_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->section_table_strings + offset, name);
}

static bool elf_read_symbol_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->symbol_table_strings + offset, name);
}

static bool elf_read_section_header(ELFFile* elf, size_t section_idx, Elf32_Shdr* section_header) {
    off_t offset = SECTION_OFFSET(elf, section_idx);
    return storage_file_seek(elf->fd, offset, true) &&
           storage_file_read(elf->fd, section_header, sizeof(Elf32_Shdr)) == sizeof(Elf32_Shdr);
}

static bool elf_read_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name) {
    if(!elf_read_section_header(elf, section_idx, section_header)) {
        return false;
    }

    if(section_header->sh_name && !elf_read_section_name(elf, section_header->sh_name, name)) {
        return false;
    }

    return true;
}

static bool elf_read_symbol(ELFFile* elf, int n, Elf32_Sym* sym, FuriString* name) {
    /* Fast path: read from cached symbol table + string table (no SD card I/O) */
    if(elf->sym_cache && (size_t)n < elf->symbol_count) {
        *sym = elf->sym_cache[n];
        if(sym->st_name && elf->str_cache && sym->st_name < elf->str_cache_size) {
            furi_string_set(name, &elf->str_cache[sym->st_name]);
            return true;
        } else if(sym->st_name == 0) {
            /* Section symbol - read section name (still needs file I/O but rare) */
            Elf32_Shdr shdr;
            return elf_read_section(elf, sym->st_shndx, &shdr, name);
        }
    }

    /* Slow path: read from file */
    bool success = false;
    off_t old = storage_file_tell(elf->fd);
    off_t pos = elf->symbol_table + n * sizeof(Elf32_Sym);
    if(storage_file_seek(elf->fd, pos, true) &&
       storage_file_read(elf->fd, sym, sizeof(Elf32_Sym)) == sizeof(Elf32_Sym)) {
        if(sym->st_name)
            success = elf_read_symbol_name(elf, sym->st_name, name);
        else {
            Elf32_Shdr shdr;
            success = elf_read_section(elf, sym->st_shndx, &shdr, name);
        }
    }
    storage_file_seek(elf->fd, old, true);
    return success;
}

static ELFSection* elf_section_of(ELFFile* elf, int index) {
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(itref->value.sec_idx == index) {
            return &itref->value;
        }
    }

    return NULL;
}

static Elf32_Addr elf_address_of(ELFFile* elf, Elf32_Sym* sym, const char* sName) {
    if(sym->st_shndx == SHN_UNDEF) {
        /* Null symbol (index 0): value is 0, used for section-relative relocations */
        if(sName[0] == '\0') {
            return 0;
        }
        Elf32_Addr addr = 0;
        uint32_t hash = elf_symbolname_hash(sName);
        if(elf->api_interface->resolver_callback(elf->api_interface, hash, &addr)) {
            return addr;
        }
        FURI_LOG_E(TAG, "  API lookup FAILED: '%s' hash=0x%08lx", sName, (unsigned long)hash);
    } else {
        ELFSection* symSec = elf_section_of(elf, sym->st_shndx);
        if(symSec) {
            Elf32_Addr addr = ((Elf32_Addr)symSec->data) + sym->st_value;
            if(symSec->is_code) {
                addr = PSRAM_DATA_TO_INST(addr);
            }
            return addr;
        }
        FURI_LOG_E(TAG, "  Section idx=%u not loaded for '%s' (bind=%u type=%u)",
            sym->st_shndx, sName, ELF32_ST_BIND(sym->st_info), ELF32_ST_TYPE(sym->st_info));
    }
    return ELF_INVALID_ADDRESS;
}

/**************************************************************************************************/
/*************************************** Xtensa Relocation ****************************************/
/**************************************************************************************************/

__attribute__((unused)) static const char* elf_reloc_type_to_str(int symt) {
#define STRCASE(name) \
    case name:        \
        return #name;
    switch(symt) {
        STRCASE(R_XTENSA_NONE)
        STRCASE(R_XTENSA_32)
        STRCASE(R_XTENSA_PLT)
        STRCASE(R_XTENSA_ASM_EXPAND)
        STRCASE(R_XTENSA_DIFF8)
        STRCASE(R_XTENSA_DIFF16)
        STRCASE(R_XTENSA_DIFF32)
        STRCASE(R_XTENSA_SLOT0_OP)
        STRCASE(R_XTENSA_SLOT0_ALT)
    default:
        return "R_<unknown>";
    }
#undef STRCASE
}

/**
 * @brief Handle R_XTENSA_SLOT0_OP relocation
 * This handles instruction-level fixups for Xtensa variable-length instructions.
 * The main cases are L32R (literal load) and CALL0/CALL4/CALL8/CALL12 (function calls).
 */
static bool elf_relocate_slot0(Elf32_Addr relAddr, Elf32_Addr symAddr, Elf32_Sword addend) {
    /* relAddr is a data-bus address (writable). But at runtime the CPU fetches
     * instructions from the instruction bus. PC-relative offsets must use
     * instruction-bus addresses for both PC and target. */
    uint32_t instPC = PSRAM_DATA_TO_INST(relAddr);
    uint32_t target = symAddr + addend;
    target = PSRAM_DATA_TO_INST(target);

    /* Use safe IRAM read for the opcode bits */
    uint8_t insn0 = elf_iram_read8((void*)relAddr);

    if(insn0 & 0x08) {
        FURI_LOG_D(TAG, "  SLOT0_OP narrow insn at 0x%08X, skipping", (unsigned int)relAddr);
        return true;
    }

    /* Wide (3-byte) instruction opcode */
    uint8_t op0 = insn0 & 0x0F;

    if(op0 == 0x01) {
        Elf32_Addr l32r_target = PSRAM_DATA_TO_INST(symAddr + addend);
        Elf32_Addr pc_aligned = (instPC + 3) & ~3;
        int32_t offset = (int32_t)(l32r_target - pc_aligned);
        if(offset & 3) return false;
        int32_t units = offset >> 2;
        if(units < -65536 || units > -1) return false;
        uint16_t imm16 = (uint16_t)(units & 0xFFFF);
        elf_iram_write8((uint8_t*)relAddr + 1, (uint8_t)(imm16 & 0xFF));
        elf_iram_write8((uint8_t*)relAddr + 2, (uint8_t)((imm16 >> 8) & 0xFF));
        return true;
    }

    if(op0 == 0x05) {
        Elf32_Addr pc_base = (instPC & ~3) + 4;
        int32_t offset = (int32_t)(target - pc_base);
        if(offset & 3) return false;
        int32_t offset18 = offset >> 2;
        if(offset18 < -131072 || offset18 > 131071) return false;
        uint8_t i0 = elf_iram_read8((void*)relAddr);
        elf_iram_write8((uint8_t*)relAddr + 0, (i0 & 0x3F) | (uint8_t)((offset18 & 0x03) << 6));
        elf_iram_write8((uint8_t*)relAddr + 1, (uint8_t)((offset18 >> 2) & 0xFF));
        elf_iram_write8((uint8_t*)relAddr + 2, (uint8_t)((offset18 >> 10) & 0xFF));
        return true;
    }

    if(op0 == 0x06) {
        uint8_t i0 = elf_iram_read8((void*)relAddr);
        uint8_t sub = (i0 >> 4) & 0x0F;
        if(sub == 0x00) {
            int32_t offset18 = (int32_t)(target - (instPC + 4));
            if(offset18 < -131072 || offset18 > 131071) return false;
            elf_iram_write8((uint8_t*)relAddr + 0, (i0 & 0x3F) | (uint8_t)((offset18 & 0x03) << 6));
            elf_iram_write8((uint8_t*)relAddr + 1, (uint8_t)((offset18 >> 2) & 0xFF));
            elf_iram_write8((uint8_t*)relAddr + 2, (uint8_t)((offset18 >> 10) & 0xFF));
            return true;
        }
    }

    FURI_LOG_D(TAG, "  SLOT0_OP: unhandled opcode 0x%02X at 0x%08X", (unsigned int)insn0, (unsigned int)relAddr);
    return true;
}

static bool
elf_relocate_symbol(ELFFile* elf, Elf32_Addr relAddr, int type, Elf32_Addr symAddr, Elf32_Sword addend) {
    UNUSED(elf);

    switch(type) {
    case R_XTENSA_32:
        elf_iram_write32((void*)relAddr, elf_iram_read32((void*)relAddr) + symAddr + addend);
        break;
    case R_XTENSA_SLOT0_OP:
        return elf_relocate_slot0(relAddr, symAddr, addend);
    case R_XTENSA_ASM_EXPAND:
        break;
    case R_XTENSA_DIFF8:
        elf_iram_write8((void*)relAddr, elf_iram_read8((void*)relAddr) + (uint8_t)(symAddr + addend));
        break;
    case R_XTENSA_DIFF16: {
        uint16_t v = elf_iram_read8((uint8_t*)relAddr) | (elf_iram_read8((uint8_t*)relAddr + 1) << 8);
        v += (uint16_t)(symAddr + addend);
        elf_iram_write8((uint8_t*)relAddr, v & 0xFF);
        elf_iram_write8((uint8_t*)relAddr + 1, (v >> 8) & 0xFF);
        break;
    }
    case R_XTENSA_DIFF32:
        elf_iram_write32((void*)relAddr, elf_iram_read32((void*)relAddr) + (uint32_t)(symAddr + addend));
        break;
    case R_XTENSA_NONE:
        break;

    default:
        FURI_LOG_E(TAG, "  Unsupported Xtensa relocation type %d", type);
        return false;
    }
    return true;
}

static bool elf_relocate(ELFFile* elf, ELFSection* s) {
    if(s->data) {
        size_t relEntries = s->rel_count;

        /* Read ALL relocation entries at once into RAM (bulk read).
         * This avoids thousands of individual 12-byte SD card reads. */
        size_t rela_size = relEntries * sizeof(Elf32_Rela);
        Elf32_Rela* rela_table = elf_psram_malloc(rela_size);
        if(!rela_table) {
            FURI_LOG_E(TAG, "Failed to alloc %u bytes for RELA table", (unsigned)rela_size);
            return false;
        }

        if(!storage_file_seek(elf->fd, s->rel_offset, true) ||
           storage_file_read(elf->fd, rela_table, rela_size) != rela_size) {
            FURI_LOG_E(TAG, "Failed to read RELA table");
            free(rela_table);
            return false;
        }

        int relocate_result = true;
        size_t resolved_ok = 0;
        size_t resolved_fail = 0;
        FuriString* symbol_name = furi_string_alloc();

        for(size_t relCount = 0; relCount < relEntries; relCount++) {
            if(relCount % RESOLVER_THREAD_YIELD_STEP == 0) {
                furi_delay_tick(1);
            }

            Elf32_Rela* rela = &rela_table[relCount];
            Elf32_Addr symAddr;

            int symEntry = ELF32_R_SYM(rela->r_info);
            int relType = ELF32_R_TYPE(rela->r_info);
            Elf32_Addr relAddr = ((Elf32_Addr)s->data) + rela->r_offset;

            if(!address_cache_get(elf->relocation_cache, symEntry, &symAddr)) {
                Elf32_Sym sym;
                furi_string_reset(symbol_name);
                if(!elf_read_symbol(elf, symEntry, &sym, symbol_name)) {
                    FURI_LOG_E(TAG, "  symbol read fail for entry %d", symEntry);
                    furi_string_free(symbol_name);
                    free(rela_table);
                    return false;
                }

                symAddr = elf_address_of(elf, &sym, furi_string_get_cstr(symbol_name));
                address_cache_put(elf->relocation_cache, symEntry, symAddr);

                if(symAddr == ELF_INVALID_ADDRESS) {
                    FURI_LOG_E(TAG, "  UNRESOLVED sym[%d] '%s'", symEntry, furi_string_get_cstr(symbol_name));
                }
            }

            if(symAddr != ELF_INVALID_ADDRESS) {
                if(!elf_relocate_symbol(elf, relAddr, relType, symAddr, rela->r_addend)) {
                    relocate_result = false;
                } else {
                    resolved_ok++;
                }
            } else {
                resolved_fail++;
                relocate_result = false;
            }
        }
        furi_string_free(symbol_name);
        free(rela_table);

        FURI_LOG_I(TAG, "Relocation: %u OK, %u FAILED out of %u",
            (unsigned)resolved_ok, (unsigned)resolved_fail, (unsigned)relEntries);

        return relocate_result;
    } else {
        FURI_LOG_D(TAG, "Section not loaded");
    }

    return false;
}

/**************************************************************************************************/
/************************************ Internal FAP interfaces *************************************/
/**************************************************************************************************/
typedef enum {
    SectionTypeUnused = 1 << 0,
    SectionTypeData = 1 << 1,
    SectionTypeRelData = 1 << 2,
    SectionTypeSymTab = 1 << 3,
    SectionTypeStrTab = 1 << 4,
    SectionTypeDebugLink = 1 << 5,
} SectionType;

static bool elf_load_debug_link(ELFFile* elf, Elf32_Shdr* section_header) {
    elf->debug_link_info.debug_link_size = section_header->sh_size;
    elf->debug_link_info.debug_link = malloc(section_header->sh_size);

    return storage_file_seek(elf->fd, section_header->sh_offset, true) &&
           storage_file_read(elf->fd, elf->debug_link_info.debug_link, section_header->sh_size) ==
               section_header->sh_size;
}

static bool str_prefix(const char* str, const char* prefix) {
    return strncmp(prefix, str, strlen(prefix)) == 0;
}

typedef enum {
    ELFLoadSectionResultSuccess,
    ELFLoadSectionResultNoMemory,
    ELFLoadSectionResultError,
} ELFLoadSectionResult;

typedef struct {
    SectionType type;
    ELFLoadSectionResult result;
} SectionTypeInfo;

static ELFLoadSectionResult
    elf_load_section_data(ELFFile* elf, ELFSection* section, Elf32_Shdr* section_header) {
    if(section_header->sh_size == 0) {
        FURI_LOG_D(TAG, "No data for section");
        return ELFLoadSectionResultSuccess;
    }

#ifdef ESP_PLATFORM
#if defined(CONFIG_IDF_TARGET_ESP32)
    if(section_header->sh_flags & SHF_EXECINSTR) {
        /* On original ESP32, PSRAM execution is highly restrictive (mapped via fixed MMU).
         * Internal IRAM is the only reliable way to execute dynamically loaded code. 
         * We allocate from IRAM and will use the DRAM mirror for relocations. */
        size_t alloc_size = (section_header->sh_size + 3) & ~3;
        FURI_LOG_I(TAG, "    EXEC heap: need %zu, free %u, largest block %u",
            alloc_size,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC));

        /* Prefer the reserved contiguous pool (big FAPs only fit here); fall back to the
         * general exec heap for small FAPs or if the pool is full/unavailable. */
        section->data = fap_exec_pool_alloc(alloc_size, section_header->sh_addralign);
        if(section->data) {
            FURI_LOG_I(TAG, "    Code section: %lu/%zu bytes in POOL @ %p",
                (unsigned long)section_header->sh_size, alloc_size, section->data);
        } else {
            section->data = heap_caps_aligned_alloc(
                section_header->sh_addralign,
                alloc_size,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
            if(section->data) {
                FURI_LOG_I(TAG, "    Code section: %lu/%zu bytes in IRAM @ %p",
                    (unsigned long)section_header->sh_size, alloc_size, section->data);
            }
        }

        if(!section->data) {
            FURI_LOG_E(TAG, "    Failed to allocate %lu bytes for IRAM code section",
                (unsigned long)section_header->sh_size);
            return ELFLoadSectionResultNoMemory;
        }
        goto section_allocated;
    }
    /* Data sections go to PSRAM */
    section->data = heap_caps_aligned_alloc(
        section_header->sh_addralign,
        section_header->sh_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if(section_header->sh_flags & SHF_EXECINSTR) {
        /* S3/Other targets handle execution from PSRAM well */
        size_t alloc_size = (section_header->sh_size + 3) & ~3;
        section->data = heap_caps_aligned_alloc(
            section_header->sh_addralign,
            alloc_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        
        if(section->data) {
            FURI_LOG_I(TAG, "    Code section: %lu/%zu bytes in PSRAM @ %p",
                (unsigned long)section_header->sh_size, alloc_size, section->data);
        }
        if(!section->data) {
            return ELFLoadSectionResultNoMemory;
        }
        goto section_allocated;
    }
    section->data = heap_caps_aligned_alloc(
        section_header->sh_addralign,
        section_header->sh_size,
        caps);
#endif
    if(!section->data) {
        /* Fallback: try any 8-bit capable memory */
        section->data = heap_caps_aligned_alloc(
            section_header->sh_addralign,
            section_header->sh_size,
            MALLOC_CAP_8BIT);
    }
#else
    section->data = aligned_malloc(section_header->sh_size, section_header->sh_addralign);
#endif

#if defined(CONFIG_IDF_TARGET_ESP32)
section_allocated:
#endif
    if(!section->data) {
        FURI_LOG_E(TAG, "    Failed to allocate %lu bytes for section",
            (unsigned long)section_header->sh_size);
        return ELFLoadSectionResultNoMemory;
    }
    section->size = section_header->sh_size;

    if(section_header->sh_type == SHT_NOBITS) {
        /* BSS section: zero memory. heap_caps_aligned_alloc does NOT zero.
         * IRAM requires 32-bit aligned access via DRAM mirror on ESP32. */
#if defined(CONFIG_IDF_TARGET_ESP32)
        if((section_header->sh_flags & SHF_EXECINSTR) &&
           esp_ptr_internal(section->data)) {
            /* IRAM: per-word mirror conversion (handles D/IRAM word inversion) */
            uint32_t* base = (uint32_t*)section->data;
            for(size_t i = 0; i < (section_header->sh_size + 3) / 4; i++) {
                elf_iram_write32(base + i, 0);
            }
        } else {
            memset(section->data, 0, section_header->sh_size);
        }
#else
        memset(section->data, 0, section_header->sh_size);
#endif
        return ELFLoadSectionResultSuccess;
    }

#if defined(CONFIG_IDF_TARGET_ESP32)
    if((section_header->sh_flags & SHF_EXECINSTR) &&
       esp_ptr_internal(section->data)) {
        /* IRAM safe-copy: read into DRAM temp buffer first, then copy
         * with 32-bit aligned writes via DRAM mirror. Direct storage_file_read 
         * into I-bus range causes LoadStoreError on ESP32. */
        size_t alloc_size = (section_header->sh_size + 3) & ~3;
        void* temp = malloc(alloc_size);
        if(!temp) return ELFLoadSectionResultError;
        if((!storage_file_seek(elf->fd, section_header->sh_offset, true)) ||
           (storage_file_read(elf->fd, temp, section_header->sh_size) != section_header->sh_size)) {
            free(temp);
            FURI_LOG_E(TAG, "    seek/read fail (IRAM safe copy)");
            return ELFLoadSectionResultError;
        }
        /* Zero any padding at the end of the temp buffer to be safe */
        if(alloc_size > section_header->sh_size) {
            memset((uint8_t*)temp + section_header->sh_size, 0, alloc_size - section_header->sh_size);
        }
        uint32_t* src = (uint32_t*)temp;
        /* Per-word mirror conversion: D/IRAM is word-inverted, so each word must be converted
         * individually (a single base conversion + ascending writes would scramble the code --
         * that was the StoreProhibited crash when loading into the D/IRAM pool). */
        uint32_t* base = (uint32_t*)section->data;
        for(size_t i = 0; i < alloc_size / 4; i++) {
            elf_iram_write32(base + i, src[i]);
        }
        free(temp);
        return ELFLoadSectionResultSuccess;
    }
    /* PSRAM code or data sections: byte-accessible, read directly */
#endif
    if((!storage_file_seek(elf->fd, section_header->sh_offset, true)) ||
       (storage_file_read(elf->fd, section->data, section_header->sh_size) !=
        section_header->sh_size)) {
        FURI_LOG_E(TAG, "    seek/read fail");
        return ELFLoadSectionResultError;
    }

    FURI_LOG_D(TAG, "0x%p", section->data);
    return ELFLoadSectionResultSuccess;
}

static SectionTypeInfo elf_preload_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name_string) {
    const char* name = furi_string_get_cstr(name_string);
    SectionTypeInfo info;

#ifdef ELF_DEBUG_LOG
    FuriString* flags_string = furi_string_alloc();
    if(section_header->sh_flags & SHF_WRITE) furi_string_cat(flags_string, "W");
    if(section_header->sh_flags & SHF_ALLOC) furi_string_cat(flags_string, "A");
    if(section_header->sh_flags & SHF_EXECINSTR) furi_string_cat(flags_string, "X");
    if(section_header->sh_flags & SHF_MERGE) furi_string_cat(flags_string, "M");
    if(section_header->sh_flags & SHF_STRINGS) furi_string_cat(flags_string, "S");
    if(section_header->sh_flags & SHF_INFO_LINK) furi_string_cat(flags_string, "I");
    if(section_header->sh_flags & SHF_LINK_ORDER) furi_string_cat(flags_string, "L");

    FURI_LOG_I(
        TAG,
        "Section %s: type: %ld, flags: %s",
        name,
        section_header->sh_type,
        furi_string_get_cstr(flags_string));
    furi_string_free(flags_string);
#endif

    // ignore Xtensa-specific metadata sections
    if(str_prefix(name, ".xtensa.") || str_prefix(name, ".rela.xtensa.") ||
       str_prefix(name, ".xt.") || str_prefix(name, ".rela.xt.") ||
       strcmp(name, ".comment") == 0) {
        FURI_LOG_D(TAG, "Ignoring metadata section '%s'", name);

        info.type = SectionTypeUnused;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load allocable section
    if(section_header->sh_flags & SHF_ALLOC) {
        ELFSection* section_p = elf_file_get_or_put_section(elf, name);
        section_p->sec_idx = section_idx;

        if(section_header->sh_type == SHT_PREINIT_ARRAY) {
            furi_assert(elf->preinit_array == NULL);
            elf->preinit_array = section_p;
        } else if(section_header->sh_type == SHT_INIT_ARRAY) {
            furi_assert(elf->init_array == NULL);
            elf->init_array = section_p;
        } else if(section_header->sh_type == SHT_FINI_ARRAY) {
            furi_assert(elf->fini_array == NULL);
            elf->fini_array = section_p;
        }

        info.type = SectionTypeData;
        section_p->is_code = (section_header->sh_flags & SHF_EXECINSTR) != 0;
        info.result = elf_load_section_data(elf, section_p, section_header);

        if(info.result != ELFLoadSectionResultSuccess) {
            FURI_LOG_E(TAG, "Error loading section '%s'", name);
        }

        return info;
    }

    // Load RELA link info section (Xtensa uses SHT_RELA)
    if(section_header->sh_flags & SHF_INFO_LINK) {
        info.type = SectionTypeRelData;

        if(str_prefix(name, ".rela")) {
            name = name + strlen(".rela");
            ELFSection* section_p = elf_file_get_or_put_section(elf, name);
            section_p->rel_count = section_header->sh_size / sizeof(Elf32_Rela);
            section_p->rel_offset = section_header->sh_offset;
            info.result = ELFLoadSectionResultSuccess;
        } else if(str_prefix(name, ".rel")) {
            // Fallback for SHT_REL (shouldn't happen on Xtensa, but be safe)
            name = name + strlen(".rel");
            ELFSection* section_p = elf_file_get_or_put_section(elf, name);
            section_p->rel_count = section_header->sh_size / sizeof(Elf32_Rel);
            section_p->rel_offset = section_header->sh_offset;
            info.result = ELFLoadSectionResultSuccess;
        } else {
            FURI_LOG_E(TAG, "Unknown link info section '%s'", name);
            info.result = ELFLoadSectionResultError;
        }

        return info;
    }

    // Load symbol table
    if(strcmp(name, ".symtab") == 0) {
        FURI_LOG_D(TAG, "Found .symtab section");
        elf->symbol_table = section_header->sh_offset;
        elf->symbol_count = section_header->sh_size / sizeof(Elf32_Sym);

        /* Cache entire symbol table in RAM for fast lookup */
        elf->sym_cache = elf_psram_malloc(section_header->sh_size);
        if(elf->sym_cache) {
            if(!storage_file_seek(elf->fd, section_header->sh_offset, true) ||
               storage_file_read(elf->fd, elf->sym_cache, section_header->sh_size) !=
                   section_header->sh_size) {
                free(elf->sym_cache);
                elf->sym_cache = NULL;
            }
        }

        info.type = SectionTypeSymTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load string table
    if(strcmp(name, ".strtab") == 0) {
        FURI_LOG_D(TAG, "Found .strtab section");
        elf->symbol_table_strings = section_header->sh_offset;

        /* Cache entire string table in RAM */
        elf->str_cache_size = section_header->sh_size;
        elf->str_cache = elf_psram_malloc(section_header->sh_size);
        if(elf->str_cache) {
            if(!storage_file_seek(elf->fd, section_header->sh_offset, true) ||
               storage_file_read(elf->fd, elf->str_cache, section_header->sh_size) !=
                   section_header->sh_size) {
                free(elf->str_cache);
                elf->str_cache = NULL;
                elf->str_cache_size = 0;
            }
        }

        info.type = SectionTypeStrTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load debug link section
    if(strcmp(name, ".gnu_debuglink") == 0) {
        FURI_LOG_D(TAG, "Found .gnu_debuglink section");
        info.type = SectionTypeDebugLink;

        if(elf_load_debug_link(elf, section_header)) {
            info.result = ELFLoadSectionResultSuccess;
            return info;
        } else {
            info.result = ELFLoadSectionResultError;
            return info;
        }
    }

    info.type = SectionTypeUnused;
    info.result = ELFLoadSectionResultSuccess;
    return info;
}

static bool elf_relocate_section(ELFFile* elf, ELFSection* section) {
    if(section->rel_count) {
        if(section->data == NULL) {
            /* Section war nicht alloziert (kein SHF_ALLOC, z.B. .debug_*).
             * Ihre Rela-Einträge sind nur für externe Tools (Debugger,
             * gdb-stub) relevant und müssen zur Laufzeit nicht aufgelöst
             * werden. Skip statt fehlschlagen — sonst bricht der Loader
             * jeden FAP ab, der mit Debug-Symbolen gebaut wurde. */
            FURI_LOG_D(TAG, "Skipping relocation: section not allocated");
            return true;
        }
        FURI_LOG_D(TAG, "Relocating section");
        return elf_relocate(elf, section);
    } else {
        FURI_LOG_D(TAG, "No relocation index"); /* Not an error */
    }
    return true;
}

static void elf_file_call_section_list(ELFSection* section, bool reverse_order) {
    if(section && section->size) {
        const uint32_t* start = section->data;
        const uint32_t* end = section->data + section->size;

        if(reverse_order) {
            while(end > start) {
                end--;
                ((void (*)(void))(*end))();
            }
        } else {
            while(start < end) {
                ((void (*)(void))(*start))();
                start++;
            }
        }
    }
}

/**************************************************************************************************/
/********************************************* Public *********************************************/
/**************************************************************************************************/

ELFFile* elf_file_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    ELFFile* elf = malloc(sizeof(ELFFile));
    memset(elf, 0, sizeof(ELFFile));
    elf->fd = storage_file_alloc(storage);
    elf->api_interface = api_interface;
    ELFSectionDict_init(elf->sections);
    elf->init_array_called = false;
    return elf;
}

void elf_file_free(ELFFile* elf) {
    if(elf->init_array_called) {
        FURI_LOG_W(TAG, "Init array was called, but fini array wasn't");
        elf_file_call_section_list(elf->fini_array, true);
    }

    // free sections data
    {
        ELFSectionDict_it_t it;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);
#ifdef ESP_PLATFORM
#if defined(CONFIG_IDF_TARGET_ESP32)
            if(fap_exec_pool_contains(itref->value.data)) {
                fap_exec_pool_free(itref->value.data);
            } else if(itref->value.data) {
                heap_caps_free(itref->value.data);
            }
#else
            if(itref->value.data) heap_caps_free(itref->value.data);
#endif
#else
            aligned_free(itref->value.data);
#endif
            free((void*)itref->key);
        }

        ELFSectionDict_clear(elf->sections);
    }

    if(elf->debug_link_info.debug_link) {
        free(elf->debug_link_info.debug_link);
    }

    if(elf->sym_cache) free(elf->sym_cache);
    if(elf->str_cache) free(elf->str_cache);

    elf_file_maybe_release_fd(elf);
    free(elf);
}

bool elf_file_open(ELFFile* elf, const char* path) {
    Elf32_Ehdr h;
    Elf32_Shdr sH;

    FURI_LOG_I(TAG, "Opening ELF file: %s", path);

    if(!storage_file_open(elf->fd, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Failed to open file: %s", path);
        return false;
    }

    if(!storage_file_seek(elf->fd, 0, true)) {
        FURI_LOG_E(TAG, "Failed to seek to start");
        return false;
    }

    uint16_t bytes_read = storage_file_read(elf->fd, &h, sizeof(h));
    if(bytes_read != sizeof(h)) {
        FURI_LOG_E(TAG, "Failed to read ELF header: got %u, expected %u", bytes_read, (unsigned)sizeof(h));
        return false;
    }

    FURI_LOG_I(
        TAG,
        "ELF header: magic=%02X%c%c%c class=%u data=%u type=%u machine=%u",
        h.e_ident[EI_MAG0],
        h.e_ident[EI_MAG1],
        h.e_ident[EI_MAG2],
        h.e_ident[EI_MAG3],
        h.e_ident[EI_CLASS],
        h.e_ident[EI_DATA],
        h.e_type,
        h.e_machine);

    FURI_LOG_I(
        TAG,
        "ELF: entry=0x%lX shoff=0x%lX shnum=%u shstrndx=%u",
        (unsigned long)h.e_entry,
        (unsigned long)h.e_shoff,
        h.e_shnum,
        h.e_shstrndx);

    /* Validate ELF header */
    if(h.e_ident[EI_MAG0] != ELFMAG0 || h.e_ident[EI_MAG1] != ELFMAG1 ||
       h.e_ident[EI_MAG2] != ELFMAG2 || h.e_ident[EI_MAG3] != ELFMAG3) {
        FURI_LOG_E(TAG, "Invalid ELF magic");
        return false;
    }

    if(h.e_ident[EI_CLASS] != ELFCLASS32) {
        FURI_LOG_E(TAG, "Not 32-bit ELF (class=%u)", h.e_ident[EI_CLASS]);
        return false;
    }

    if(h.e_ident[EI_DATA] != ELFDATA2LSB) {
        FURI_LOG_E(TAG, "Not little-endian ELF (data=%u)", h.e_ident[EI_DATA]);
        return false;
    }

    if(h.e_type != ET_REL) {
        FURI_LOG_E(TAG, "Not relocatable ELF (type=%u, expected %u)", h.e_type, ET_REL);
        return false;
    }

    if(h.e_shoff == 0 || h.e_shnum == 0) {
        FURI_LOG_E(TAG, "No section headers (shoff=%lu, shnum=%u)", (unsigned long)h.e_shoff, h.e_shnum);
        return false;
    }

    if(h.e_shstrndx >= h.e_shnum) {
        FURI_LOG_E(TAG, "Invalid shstrndx=%u (shnum=%u)", h.e_shstrndx, h.e_shnum);
        return false;
    }

    off_t shstr_offset = h.e_shoff + h.e_shstrndx * sizeof(sH);
    FURI_LOG_I(TAG, "Reading section string table header at offset 0x%lX", (unsigned long)shstr_offset);

    if(!storage_file_seek(elf->fd, shstr_offset, true) ||
       storage_file_read(elf->fd, &sH, sizeof(Elf32_Shdr)) != sizeof(Elf32_Shdr)) {
        FURI_LOG_E(TAG, "Failed to read section string table header");
        return false;
    }

    FURI_LOG_I(TAG, "Section string table: offset=0x%lX size=%lu", (unsigned long)sH.sh_offset, (unsigned long)sH.sh_size);

    elf->entry = h.e_entry;
    elf->sections_count = h.e_shnum;
    elf->section_table = h.e_shoff;
    elf->section_table_strings = sH.sh_offset;

    FURI_LOG_I(TAG, "ELF file opened OK: %u sections", (unsigned)elf->sections_count);
    return true;
}

ElfLoadSectionTableResult elf_file_load_section_table(ELFFile* elf) {
    SectionType loaded_sections = 0;
    FuriString* name = furi_string_alloc();
    ElfLoadSectionTableResult result = ElfLoadSectionTableResultSuccess;

    FURI_LOG_D(TAG, "Scan ELF sections...");

    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        Elf32_Shdr section_header;

        furi_string_reset(name);
        if(!elf_read_section(elf, section_idx, &section_header, name)) {
            loaded_sections = 0;
            break;
        }

        FURI_LOG_D(
            TAG, "Preloading data for section #%d %s", section_idx, furi_string_get_cstr(name));
        SectionTypeInfo section_type_info =
            elf_preload_section(elf, section_idx, &section_header, name);
        loaded_sections |= section_type_info.type;

        if(section_type_info.result != ELFLoadSectionResultSuccess) {
            if(section_type_info.result == ELFLoadSectionResultNoMemory) {
                FURI_LOG_E(TAG, "Not enough memory");
                result = ElfLoadSectionTableResultNoMemory;
            } else if(section_type_info.result == ELFLoadSectionResultError) {
                FURI_LOG_E(TAG, "Error loading section");
                result = ElfLoadSectionTableResultError;
            }

            loaded_sections = 0;
            break;
        }
    }

    furi_string_free(name);

    if(result != ElfLoadSectionTableResultSuccess) {
        return result;
    } else {
        bool sections_valid =
            IS_FLAGS_SET(loaded_sections, SectionTypeSymTab | SectionTypeStrTab);
        if(sections_valid) {
            return ElfLoadSectionTableResultSuccess;
        } else {
            FURI_LOG_E(TAG, "No valid sections found");
            return ElfLoadSectionTableResultError;
        }
    }
}

ElfProcessSectionResult elf_process_section(
    ELFFile* elf,
    const char* name,
    ElfProcessSection* process_section,
    void* context) {
    FURI_LOG_I(TAG, "Looking for section '%s' in %u sections", name, (unsigned)elf->sections_count);

    ElfProcessSectionResult result = ElfProcessSectionResultNotFound;
    FuriString* section_name = furi_string_alloc();
    Elf32_Shdr section_header;

    // find section
    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        furi_string_reset(section_name);
        if(!elf_read_section(elf, section_idx, &section_header, section_name)) {
            FURI_LOG_E(TAG, "Failed to read section #%u", (unsigned)section_idx);
            break;
        }

        FURI_LOG_D(TAG, "  Section #%u: '%s'", (unsigned)section_idx, furi_string_get_cstr(section_name));

        if(furi_string_cmp(section_name, name) == 0) {
            FURI_LOG_I(TAG, "Found section '%s' at idx %u, offset=0x%lX size=%lu",
                name, (unsigned)section_idx,
                (unsigned long)section_header.sh_offset,
                (unsigned long)section_header.sh_size);
            result = ElfProcessSectionResultCannotProcess;
            break;
        }
    }

    if(result == ElfProcessSectionResultNotFound) {
        FURI_LOG_W(TAG, "Section '%s' not found", name);
    } else {
        if(process_section(elf->fd, section_header.sh_offset, section_header.sh_size, context)) {
            FURI_LOG_I(TAG, "Section '%s' processed OK", name);
            result = ElfProcessSectionResultSuccess;
        } else {
            FURI_LOG_E(TAG, "Section '%s' processing failed", name);
            result = ElfProcessSectionResultCannotProcess; //-V1048
        }
    }

    furi_string_free(section_name);

    return result;
}

ELFFileLoadStatus elf_file_load_sections(ELFFile* elf) {
    furi_check(elf->fd != NULL);
    ELFFileLoadStatus status = ELFFileLoadStatusSuccess;
    ELFSectionDict_it_t it;

    AddressCache_init(elf->relocation_cache);

    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        FURI_LOG_D(TAG, "Relocating section '%s'", itref->key);
        if(!elf_relocate_section(elf, &itref->value)) {
            FURI_LOG_E(TAG, "Error relocating section '%s'", itref->key);
            status = ELFFileLoadStatusMissingImports;
        }
    }

    /* Fixing up entry point */
    if(status == ELFFileLoadStatusSuccess) {
        ELFSection* text_section = elf_file_get_section(elf, ".text");

        if(text_section == NULL) {
            FURI_LOG_E(TAG, "No .text section found");
            status = ELFFileLoadStatusUnspecifiedError;
        } else {
            elf->entry += (uint32_t)text_section->data;
            /* Convert to instruction bus address for code execution */
            elf->entry = PSRAM_DATA_TO_INST(elf->entry);
            FURI_LOG_I(TAG, "Entry point: 0x%08lX", (unsigned long)elf->entry);
        }
    }

    FURI_LOG_D(TAG, "Relocation cache size: %u", AddressCache_size(elf->relocation_cache));
    AddressCache_clear(elf->relocation_cache);

    {
        size_t total_size = 0;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
            total_size += itref->value.size;
        }
        FURI_LOG_I(TAG, "Total size of loaded sections: %zu", total_size);
    }

    elf_file_maybe_release_fd(elf);
    return status;
}

void elf_file_call_init(ELFFile* elf) {
    furi_check(!elf->init_array_called);
    elf_file_call_section_list(elf->preinit_array, false);
    elf_file_call_section_list(elf->init_array, false);
    elf->init_array_called = true;
}

bool elf_file_is_init_complete(ELFFile* elf) {
    return elf->init_array_called;
}

void* elf_file_get_entry_point(ELFFile* elf) {
    furi_check(elf->init_array_called);
    return (void*)elf->entry;
}

void elf_file_call_fini(ELFFile* elf) {
    furi_check(elf->init_array_called);
    elf_file_call_section_list(elf->fini_array, true);
    elf->init_array_called = false;
}

const ElfApiInterface* elf_file_get_api_interface(ELFFile* elf_file) {
    return elf_file->api_interface;
}

void elf_file_init_debug_info(ELFFile* elf, ELFDebugInfo* debug_info) {
    // set entry
    debug_info->entry = elf->entry;

    // copy debug info
    memcpy(&debug_info->debug_link_info, &elf->debug_link_info, sizeof(ELFDebugLinkInfo));

    // init mmap
    debug_info->mmap_entry_count = ELFSectionDict_size(elf->sections);
    debug_info->mmap_entries = malloc(sizeof(ELFMemoryMapEntry) * debug_info->mmap_entry_count);
    uint32_t mmap_entry_idx = 0;

    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);

        const void* data_ptr = itref->value.data;
        if(data_ptr) {
            ELFMemoryMapEntry* entry = &debug_info->mmap_entries[mmap_entry_idx];
            entry->address = (uint32_t)data_ptr;
            entry->size = itref->value.size;
            entry->name = itref->key;
            mmap_entry_idx++;
        }
    }
}

void elf_file_clear_debug_info(ELFDebugInfo* debug_info) {
    memset(&debug_info->debug_link_info, 0, sizeof(ELFDebugLinkInfo));

    if(debug_info->mmap_entries) {
        free(debug_info->mmap_entries);
        debug_info->mmap_entries = NULL;
    }

    debug_info->mmap_entry_count = 0;
}
