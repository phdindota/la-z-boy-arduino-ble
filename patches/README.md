# NimBLE Library Patch

## Why This Patch is Needed

La-Z-Boy power recliners hardcode GATT handle positions in their firmware. The chair expects the notify VALUE characteristic at handle `0x000D`. If it's at any other handle, the chair connects and bonds but completely ignores all commands.

NimBLE-Arduino's built-in GATT service includes a **Service Changed** characteristic (UUID `0x2A05`) that consumes 3 handle slots (declaration + value + CCCD descriptor). These 3 extra handles push our La-Z-Boy service too far down the handle table.

## What the Patch Does

Removes the Service Changed characteristic from the GATT service definition. This is a 6-line change in one file:

1. Comments out the Service Changed characteristic struct (saves 3 handles)
2. Adds a safety guard in `ble_svc_gatt_changed()` so nothing crashes

The Service Changed characteristic is used to tell BLE clients that the server's GATT table has changed. Since our GATT table is static (it never changes at runtime), removing this characteristic has no functional impact.

## How to Apply

### Option A: Run the setup script (Linux/macOS)

```bash
./setup.sh
```

### Option B: Copy the pre-patched file

```bash
# Backup original
cp /path/to/NimBLE-Arduino/src/nimble/nimble/host/services/gatt/src/ble_svc_gatt.c \
   /path/to/NimBLE-Arduino/src/nimble/nimble/host/services/gatt/src/ble_svc_gatt.c.bak

# Copy patched version
cp patches/ble_svc_gatt.c.patched \
   /path/to/NimBLE-Arduino/src/nimble/nimble/host/services/gatt/src/ble_svc_gatt.c
```

Typical NimBLE path on Linux:
```
~/Arduino/libraries/NimBLE-Arduino/src/nimble/nimble/host/services/gatt/src/ble_svc_gatt.c
```

### Option C: Manual edit

Open `ble_svc_gatt.c` and make two changes:

**Change 1** — Comment out the Service Changed characteristic (around line 63):

```c
        // BEFORE:
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = BLE_UUID16_DECLARE(BLE_SVC_GATT_CHR_SERVICE_CHANGED_UUID16),
            .access_cb = ble_svc_gatt_access,
            .val_handle = &ble_svc_gatt_changed_val_handle,
            .flags = BLE_GATT_CHR_F_INDICATE,
        },

        // AFTER:
        .characteristics = (struct ble_gatt_chr_def[]) {
        /*{
            .uuid = BLE_UUID16_DECLARE(BLE_SVC_GATT_CHR_SERVICE_CHANGED_UUID16),
            .access_cb = ble_svc_gatt_access,
            .val_handle = &ble_svc_gatt_changed_val_handle,
            .flags = BLE_GATT_CHR_F_INDICATE,
        },*/
```

**Change 2** — Guard the `ble_svc_gatt_changed()` function:

```c
        // BEFORE:
        void
        ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle)
        {
            ble_svc_gatt_start_handle = start_handle;

        // AFTER:
        void
        ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle)
        {
            if (ble_svc_gatt_changed_val_handle == 0) return;
            ble_svc_gatt_start_handle = start_handle;
```

## How to Restore

```bash
./setup.sh --restore
```

Or manually copy the `.bak` file back.

## Tested With

- NimBLE-Arduino 2.3.9
- ESP32 Arduino Core 3.3.7
- ESP32-WROOM-32 / ESP32 Dev Module

## Handle Layout

Without patch (13 handles before our service — too many):
```
0x0001  GAP Service
0x0002  Device Name decl
0x0003  Device Name value
0x0004  Appearance decl
0x0005  Appearance value
0x0006  GATT Service
0x0007  Service Changed decl      ← removed by patch
0x0008  Service Changed value     ← removed by patch
0x0009  Service Changed CCCD      ← removed by patch
0x000A  Server Supported Feat decl
0x000B  Server Supported Feat value
0x000C  Client Supported Feat decl
0x000D  Client Supported Feat value
0x000E  La-Z-Boy Service          ← too late!
```

With patch (10 handles — correct):
```
0x0001  GAP Service
0x0002  Device Name decl
0x0003  Device Name value
0x0004  Appearance decl
0x0005  Appearance value
0x0006  GATT Service
0x0007  Server Supported Feat decl
0x0008  Server Supported Feat value
0x0009  Client Supported Feat decl
0x000A  Client Supported Feat value
0x000B  La-Z-Boy Service          ← correct!
0x000C  Notify decl
0x000D  Notify VALUE              ← target!
0x000E  Notify CCCD               ← target!
```
