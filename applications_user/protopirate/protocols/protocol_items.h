// protocols/protocol_items.h
#pragma once

#include <lib/subghz/types.h>

#include "scher_khan.h"
// kia_generic.h + kia_v0..v6.h stripped on classic ESP32 to fit the exec pool
#include "ford_v0.h"
#include "fiat_v0.h"
#include "fiat_v1.h"
#include "mazda_v0.h"
#include "mitsubishi_v0.h"
#include "porsche_touareg.h"
#include "subaru.h"
#include "suzuki.h"
// vag.h / star_line.h / psa.h stripped on classic ESP32 to fit the exec pool

extern const SubGhzProtocolRegistry protopirate_protocol_registry;

// Timing information for protocol analysis
typedef struct {
    const char* name;
    uint32_t te_short;
    uint32_t te_long;
    uint32_t te_delta;
    uint32_t min_count_bit;
} ProtoPirateProtocolTiming;

// Get timing info for a protocol by name (returns NULL if not found)
const ProtoPirateProtocolTiming* protopirate_get_protocol_timing(const char* protocol_name);

// Get timing info by index (for iteration)
const ProtoPirateProtocolTiming* protopirate_get_protocol_timing_by_index(size_t index);

// Get number of protocols with timing info
size_t protopirate_get_protocol_timing_count(void);
