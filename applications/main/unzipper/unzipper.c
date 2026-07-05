#include <furi.h>
#include <gui/gui.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#include "miniz.h"

#define TAG "Unzipper"


#pragma pack(push, 1)
typedef struct {
    uint32_t signature;
    uint16_t diskNumber;
    uint16_t startDiskNumber;
    uint16_t entriesOnDisk;
    uint16_t entriesInDirectory;
    uint32_t directorySize;
    uint32_t directoryOffset;
    uint16_t commentLength;
} EocdRecord;

typedef struct {
    uint32_t signature;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
    uint16_t fileCommentLength;
    uint16_t diskNumberStart;
    uint16_t internalFileAttributes;
    uint32_t externalFileAttributes;
    uint32_t localHeaderOffset;
} CdRecord;

typedef struct {
    uint32_t signature;
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
} LocalHeader;
#pragma pack(pop)

static bool extract_file(Storage* storage, File* file, const char* out_dir, CdRecord* cd) {
    LocalHeader lh;
    storage_file_seek(file, cd->localHeaderOffset, true);
    storage_file_read(file, &lh, sizeof(lh));

    char filename[256] = {0};
    uint16_t name_len = MIN(lh.filenameLength, (uint16_t)255);
    storage_file_read(file, filename, name_len);

    storage_file_seek(file, lh.extraFieldLength, false); // Skip extra field

    // Create dir or file
    FuriString* full_path = furi_string_alloc_printf("%s/%s", out_dir, filename);

    // Check if exists
    if(storage_common_exists(storage, furi_string_get_cstr(full_path))) {
        FURI_LOG_D(TAG, "Skipping existing: %s", furi_string_get_cstr(full_path));
        furi_string_free(full_path);
        return true;
    }

    bool is_dir = filename[name_len - 1] == '/';
    
    if(is_dir) {
        storage_simply_mkdir(storage, furi_string_get_cstr(full_path));
        furi_string_free(full_path);
        return true;
    }

    FuriString* parent = furi_string_alloc();
    const char* full_cstr = furi_string_get_cstr(full_path);
    const char* last_slash = strrchr(full_cstr, '/');
    if(last_slash) {
        furi_string_set_strn(parent, full_cstr, last_slash - full_cstr);
    }
    storage_simply_mkdir(storage, furi_string_get_cstr(parent));
    furi_string_free(parent);

    File* out = storage_file_alloc(storage);
    bool res = storage_file_open(out, furi_string_get_cstr(full_path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    furi_string_free(full_path);

    if(!res) {
        storage_file_free(out);
        return false;
    }

    if(lh.compressionMethod == 0) {
        // Store
        uint32_t remain = lh.compressedSize;
        uint8_t buf[512];
        while(remain > 0) {
            uint16_t to_read = remain > 512 ? 512 : remain;
            storage_file_read(file, buf, to_read);
            storage_file_write(out, buf, to_read);
            remain -= to_read;
        }
    } else if(lh.compressionMethod == 8) {
        // Deflate
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        inflateInit2(&strm, -15); // Raw deflate

        uint8_t in_buf[512];
        uint8_t out_buf[512];
        uint32_t remain = lh.compressedSize;

        while(remain > 0) {
            uint16_t to_read = remain > 512 ? 512 : remain;
            storage_file_read(file, in_buf, to_read);
            remain -= to_read;

            strm.avail_in = to_read;
            strm.next_in = in_buf;

            do {
                strm.avail_out = 512;
                strm.next_out = out_buf;
                inflate(&strm, Z_NO_FLUSH);
                uint16_t have = 512 - strm.avail_out;
                if(have > 0) storage_file_write(out, out_buf, have);
            } while(strm.avail_out == 0);
        }
        inflateEnd(&strm);
    }

    storage_file_close(out);
    storage_file_free(out);
    return true;
}

static void show_dialog(const char* text) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
}

int32_t unzipper_app(void* p) {
    UNUSED(p);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    
    FuriString* file_path = furi_string_alloc_printf("%s", "/ext");
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, ".zip", NULL);

    bool res = dialog_file_browser_show(dialogs, file_path, file_path, &browser_options);
    furi_record_close(RECORD_DIALOGS);

    if(!res) {
        furi_string_free(file_path);
        furi_record_close(RECORD_STORAGE);
        return 0;
    }

    // Get output directory (same as zip)
    FuriString* out_dir = furi_string_alloc();
    const char* full_path_cstr = furi_string_get_cstr(file_path);
    const char* last_slash_idx = strrchr(full_path_cstr, '/');
    if(last_slash_idx) {
        furi_string_set_strn(out_dir, full_path_cstr, last_slash_idx - full_path_cstr);
    } else {
        furi_string_set(out_dir, "/ext");
    }

    show_dialog("Extracting... Please wait.");

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, furi_string_get_cstr(file_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Find EOCD (assume no comment)
        uint64_t size = storage_file_size(file);
        storage_file_seek(file, size - 22, true);
        
        EocdRecord eocd;
        storage_file_read(file, &eocd, sizeof(eocd));
        
        if(eocd.signature == 0x06054b50) {
            storage_file_seek(file, eocd.directoryOffset, true);
            
            for(uint16_t i=0; i<eocd.entriesInDirectory; i++) {
                CdRecord cd;
                storage_file_read(file, &cd, sizeof(cd));
                if(cd.signature != 0x02014b50) break;
                
                uint64_t next_cd = storage_file_tell(file) + cd.filenameLength + cd.extraFieldLength + cd.fileCommentLength;
                
                extract_file(storage, file, furi_string_get_cstr(out_dir), &cd);
                
                storage_file_seek(file, next_cd, true);
            }
            show_dialog("Extraction Complete!");
        } else {
            show_dialog("Invalid ZIP file or has comment.");
        }
    } else {
        show_dialog("Cannot open file.");
    }
    
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(file_path);
    furi_string_free(out_dir);
    furi_record_close(RECORD_STORAGE);

    return 0;
}
