#include "psa.h"

/* The PSA protocol decoder (psa.c) is stripped on the classic ESP32 build: its .text
 * doesn't fit the exec pool. PSA is therefore not registered, so a PSA signal is never
 * decoded and the brute-force path in scene_receiver_info.c can never trigger
 * (psa_item_needs_bruteforce() stays false). These stubs only exist so the references
 * still resolve at link/load time. */

bool psa_bf_state_from_flipper_format(PsaBfState* state, FlipperFormat* ff) {
    (void)state;
    (void)ff;
    return false;
}

int32_t psa_brute_force_thread_entry(void* arg) {
    (void)arg;
    return 0;
}
