cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# ---------------------------------------------------------------------------
# Patch libnet80211.a to allow raw TX of deauth/disassoc management frames.
# The closed-source WiFi blob rejects frame types 0xC0/0xA0; we patch
# ieee80211_raw_frame_sanity_check() to return 0. Idempotent — skips if
# already patched.  Runs at configure time (before library path resolution).
# ---------------------------------------------------------------------------
# The unified xtensa-esp-elf toolchain handles every Xtensa target (esp32,
# esp32s3, ...); the script derives objdump from the objcopy name itself.
find_program(_xt_ar NAMES xtensa-esp-elf-ar xtensa-esp32-elf-ar
    PATHS "$ENV{HOME}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin" NO_DEFAULT_PATH)
find_program(_xt_objcopy NAMES xtensa-esp-elf-objcopy xtensa-esp32-elf-objcopy
    PATHS "$ENV{HOME}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin" NO_DEFAULT_PATH)

# Patch every Xtensa WiFi blob we might link. The M5StickC Plus2 is a CLASSIC
# esp32, so the esp32 lib is the one that actually matters here; esp32s3 is kept
# patched too in case the board target changes. Idempotent.
foreach(_chip esp32 esp32s3)
    set(_wifi_lib "$ENV{IDF_PATH}/components/esp_wifi/lib/${_chip}/libnet80211.a")
    if(_xt_ar AND _xt_objcopy AND EXISTS "${_wifi_lib}")
        execute_process(
            COMMAND "${CMAKE_SOURCE_DIR}/tools/patch_wifi_lib.py"
                "${_wifi_lib}" "${_xt_objcopy}" "${_xt_ar}" "${_wifi_lib}"
            RESULT_VARIABLE _patch_rc OUTPUT_VARIABLE _patch_out ERROR_VARIABLE _patch_err)
        if(_patch_rc EQUAL 0)
            message(STATUS "WiFi lib (${_chip}): ${_patch_out}")
        else()
            message(WARNING "WiFi lib patch failed (${_chip}): ${_patch_err}")
        endif()
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(furi_esp32)
