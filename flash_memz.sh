#!/usr/bin/env bash
# flash_memz.sh
# Flash the M5StickC Plus2 with the MEMZ prank:
#   ota_0 = Flipper Zero firmware   (default boot)
#   ota_1 = MEMZ Trojan firmware     (booted via "Bruce" menu)
#
# Usage: ./flash_memz.sh [PORT]
#   PORT defaults to /dev/ttyACM0

set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-/dev/ttyACM0}"
IDF_DIR="${HOME}/esp/esp-idf"
MEMZ_BIN="${HOME}/memz_mbr/build/memz_mbr.bin"
BUILD_DIR="build"

if [ ! -f "${MEMZ_BIN}" ]; then
    echo "ERROR: MEMZ firmware not found at ${MEMZ_BIN}"
    echo "Build it first: cd ~/memz_mbr && idf.py build"
    exit 1
fi

source "${IDF_DIR}/export.sh" > /dev/null 2>&1

echo "=== Step 1: Erasing entire flash (partition table changed) ==="
esptool.py --chip esp32 --port "${PORT}" erase_flash

echo ""
echo "=== Step 2: Flashing bootloader + partition table + otadata + Flipper firmware ==="
idf.py -p "${PORT}" flash

echo ""
echo "=== Step 3: Flashing MEMZ payload to ota_1 partition (offset 0x3A0000) ==="
esptool.py --chip esp32 --port "${PORT}" write_flash 0x3A0000 "${MEMZ_BIN}"

echo ""
echo "=== DONE ==="
echo "The device will boot into the Flipper firmware (ota_0)."
echo "Select 'Bruce' from the menu to switch to the MEMZ payload (ota_1)."
echo " Press and hold BOOT + RST to enter download mode and escape."
