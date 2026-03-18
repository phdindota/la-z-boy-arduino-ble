#!/bin/bash
#
# La-Z-Boy BLE Controller - NimBLE Library Patch Script
#
# This script patches NimBLE-Arduino to remove the GATT Service Changed
# characteristic, freeing 3 handle slots so the La-Z-Boy notify VALUE
# characteristic lands at handle 0x000D (required by the chair firmware).
#
# Usage:
#   ./setup.sh                  # Auto-detect NimBLE location
#   ./setup.sh /path/to/NimBLE  # Specify NimBLE library path
#   ./setup.sh --restore        # Restore original file from backup
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_FILE="$SCRIPT_DIR/patches/nimble_remove_service_changed.patch"
TARGET_FILE="nimble/nimble/host/services/gatt/src/ble_svc_gatt.c"

# Find NimBLE library
find_nimble() {
    local search_paths=(
        "$1"  # User-provided path
        "$HOME/Arduino/libraries/NimBLE-Arduino/src"
        "$HOME/Documents/Arduino/libraries/NimBLE-Arduino/src"
        "$HOME/.arduino15/packages/esp32/libraries/NimBLE-Arduino/src"
    )

    for path in "${search_paths[@]}"; do
        if [ -n "$path" ] && [ -f "$path/$TARGET_FILE" ]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

# Restore original
if [ "$1" = "--restore" ]; then
    NIMBLE_PATH=$(find_nimble "$2") || {
        echo -e "${RED}Cannot find NimBLE-Arduino library.${NC}"
        echo "Specify path: $0 --restore /path/to/NimBLE-Arduino/src"
        exit 1
    }
    BACKUP="$NIMBLE_PATH/$TARGET_FILE.bak"
    if [ -f "$BACKUP" ]; then
        cp "$BACKUP" "$NIMBLE_PATH/$TARGET_FILE"
        echo -e "${GREEN}Restored original ble_svc_gatt.c from backup.${NC}"
    else
        echo -e "${RED}No backup found at $BACKUP${NC}"
        exit 1
    fi
    exit 0
fi

echo "============================================"
echo "  La-Z-Boy BLE Controller - NimBLE Patch"
echo "============================================"
echo ""

# Find library
NIMBLE_PATH=$(find_nimble "$1") || {
    echo -e "${RED}Cannot find NimBLE-Arduino library.${NC}"
    echo ""
    echo "Install it first:"
    echo "  Arduino IDE: Sketch > Include Library > Manage Libraries > search 'NimBLE'"
    echo ""
    echo "Or specify the path:"
    echo "  $0 /path/to/NimBLE-Arduino/src"
    exit 1
}

FULL_PATH="$NIMBLE_PATH/$TARGET_FILE"
echo -e "Found NimBLE at: ${GREEN}$NIMBLE_PATH${NC}"
echo ""

# Check if already patched
if grep -q "Service Changed characteristic REMOVED" "$FULL_PATH" 2>/dev/null; then
    echo -e "${YELLOW}Already patched!${NC} Nothing to do."
    echo "Use '$0 --restore' to undo the patch."
    exit 0
fi

# Backup
cp "$FULL_PATH" "$FULL_PATH.bak"
echo "Backed up original to: $FULL_PATH.bak"

# Apply patch via sed (more portable than patch utility)
# Change 1: Comment out Service Changed characteristic
sed -i '/.characteristics = (struct ble_gatt_chr_def\[\]) { {/{
s/{ {/{\
        \/* Service Changed characteristic REMOVED to save 3 GATT handles.\
         * La-Z-Boy chairs hardcode handle positions and expect the notify\
         * VALUE characteristic at handle 0x000D.\
         *\/\
        \/*{/
}' "$FULL_PATH"

# Find and comment out the closing of Service Changed block
sed -i '/\.flags = BLE_GATT_CHR_F_INDICATE,/{
n
s/^        },/        },*\//
}' "$FULL_PATH"

# Change 2: Guard ble_svc_gatt_changed against removed characteristic
sed -i '/^ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle)/{
n
s/{/{\
    \/* Skip if Service Changed characteristic was removed *\/\
    if (ble_svc_gatt_changed_val_handle == 0) return;/
}' "$FULL_PATH"

echo ""
echo -e "${GREEN}Patch applied successfully!${NC}"
echo ""
echo "What was changed:"
echo "  - Removed GATT Service Changed characteristic (saves 3 handles)"
echo "  - Added safety guard in ble_svc_gatt_changed()"
echo ""
echo "Next steps:"
echo "  1. Open src/lazboy_ble_controller.ino in Arduino IDE"
echo "  2. Select your ESP32 board and upload"
echo "  3. Open Serial Monitor (115200 baud)"
echo "  4. Check that Notify VALUE shows 0x000D"
echo "  5. Wipe bonds and factory reset the chair if needed"
echo ""
echo "To undo: $0 --restore"
