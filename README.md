# La-Z-Boy BLE Wireless Remote Controller

Control La-Z-Boy power recliners via BLE using an ESP32, with optional MQTT integration for Home Assistant.

The ESP32 emulates the La-Z-Boy wireless remote by acting as a BLE peripheral. The chair (which acts as a BLE central) connects to the ESP32 and accepts motor/preset commands over GATT notifications — exactly as it would from the original remote.

## How It Works

La-Z-Boy power recliners use a BLE architecture where the **chair is the central** and the **remote is the peripheral**. This is the reverse of most BLE setups. The remote advertises, the chair connects, bonds, and then listens for GATT notifications on specific characteristics.

This firmware:

1. Spoofs the original remote's BLE MAC address
2. Advertises with the same service UUID and manufacturer data
3. Creates a GATT server with the correct service/characteristic layout (including a padding service for handle alignment)
4. Accepts the chair's connection and completes bonding
5. Sends command packets as notifications on two characteristics simultaneously

Commands are 3–4 byte sequences where byte 2 encodes the action: `0x09` = press, `0x03` = hold, `0x0A` = release.

## Hardware Required

- **ESP32** development board (ESP32-WROOM or ESP32-DevKitC recommended)
- USB cable for flashing and serial monitor
- (Optional) Permanent 5V power supply for always-on operation

No additional components are needed — just the ESP32.

## Software Dependencies

This project uses the **Arduino framework** with the following libraries:

| Library | Purpose |
|---------|---------|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | BLE stack (lighter and more reliable than the default ESP32 BLE library) |
| WiFi (built-in) | Optional WiFi connectivity |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | Optional MQTT client |

Install via Arduino Library Manager or PlatformIO.

## Setup

### 1. Find Your Remote's MAC Address

You need the BLE MAC address of your **original La-Z-Boy remote**. Use a BLE scanner app (e.g., nRF Connect) to find a device advertising as `LZB-SHL` or similar.

> **Important:** The ESP32 adds +2 to the base MAC for BLE. If your remote's BLE MAC ends in `0x50`, set the last byte in `REMOTE_MAC` to `0x4E` (i.e., `0x50 - 2`).

### 2. Unpair the Original Remote

The chair can only bond with one remote at a time. You need to **factory reset the chair's pairing**:

1. Unplug the chair from power
2. Hold the Home button on the original remote
3. Plug the chair back in while still holding Home
4. Release after ~5 seconds — the chair should be in pairing mode

### 3. Configure the Sketch

Edit the configuration section at the top of `src/lazboy_ble_controller.ino`:

```cpp
// Your remote's MAC (remember: subtract 2 from the last byte)
uint8_t REMOTE_MAC[6] = {0x00, 0x01, 0x90, 0x82, 0xB2, 0x4E};

// Optional: WiFi + MQTT for Home Assistant
const char* WIFI_SSID     = "YourNetwork";
const char* WIFI_PASSWORD  = "YourPassword";
const char* MQTT_SERVER    = "192.168.1.100";
```

Leave WiFi/MQTT fields empty to run in serial-only mode.

### 4. Flash and Run

1. Open the sketch in Arduino IDE or PlatformIO
2. Select your ESP32 board
3. Upload
4. Open Serial Monitor at 115200 baud
5. Power cycle the chair — it should connect and bond within a few seconds

You'll see:

```
✓ Chair connected: xx:xx:xx:xx:xx:xx
✓ Authenticated (bonded: yes)
✓ Ready for commands!
```

## Commands

### Serial / MQTT

Send commands via Serial Monitor or publish to the MQTT topic (`lazboy/command` by default).

| Command | Action |
|---------|--------|
| `home` | Activate Home preset |
| `flat` | Activate Flat preset |
| `tv` | Activate TV preset |
| `save_flat` | Save current position as Flat preset (3.5s hold) |
| `save_tv` | Save current position as TV preset (3.5s hold) |
| `head_up` / `head_down` | Head motor |
| `recline_up` / `recline_down` | Recline motor |
| `feet_up` / `feet_down` | Feet motor (alias for recline) |
| `lumbar_up` / `lumbar_down` | Lumbar motor |
| `status` | Print connection status |
| `wipe` | Clear BLE bonds (re-pair required) |
| `help` | Show command list |

### Timed Commands

Append `:ms` to any motor command to run it for a specific duration:

```
head_up:500      # Half second
recline_down:3000 # Three seconds
lumbar_up:250     # Quarter second
```

## Home Assistant Integration

### MQTT

With WiFi and MQTT configured, you can control the chair from Home Assistant using MQTT publishes:

```yaml
# Example: Button in HA
mqtt:
  button:
    - name: "La-Z-Boy Home"
      command_topic: "lazboy/command"
      payload_press: "home"

    - name: "La-Z-Boy Flat"
      command_topic: "lazboy/command"
      payload_press: "flat"

    - name: "La-Z-Boy TV"
      command_topic: "lazboy/command"
      payload_press: "tv"
```

### Automation Example

```yaml
automation:
  - alias: "Movie Time - Recline Chair"
    trigger:
      - platform: state
        entity_id: media_player.living_room_tv
        to: "playing"
    action:
      - service: mqtt.publish
        data:
          topic: "lazboy/command"
          payload: "tv"
```

## BLE Protocol Details

### Service & Characteristics

| UUID | Role | Properties |
|------|------|------------|
| `826c364a-1005-11e8-b642-0ed5f89f718b` | Primary Service | — |
| `826c3d34-...` | Notify Characteristic | Read, Notify |
| `826c3fc8-...` | Write Characteristic | Write, Write No Response |
| `826c4248-...` | Bidirectional Characteristic | Write, Write No Response, Notify |
| `826c44c8-...` | Read Characteristic | Read |

### Command Packet Format

Commands are sent as 20-byte packets (padded with zeros). The first 3–4 bytes carry the command:

| Byte | Meaning |
|------|---------|
| 0 | Command ID (motor/preset identifier) |
| 1 | Action: `0x09` = press, `0x03` = hold, `0x0A` = release |
| 2 | Parameter (direction/flags) |
| 3 | Modifier (used in save sequences) |

### Motor Commands

| Command | Byte 0 | Byte 2 (press) |
|---------|--------|-----------------|
| Recline Up | `0x11` | `0x02` |
| Recline Down | `0x12` | `0x01` |
| Head Up | `0x21` | `0x40` |
| Head Down | `0x22` | `0x80` |
| Lumbar Up | `0x41` | `0x04` |
| Lumbar Down | `0x42` | `0x08` |

### Preset Commands

| Command | Byte 0 |
|---------|--------|
| Home | `0x93` |
| TV | `0x94` |
| Flat | `0x98` |

### Save Preset Sequence

Saving a preset uses a 3-packet sequence with precise timing (captured from the original remote):

```
T=0.0s  → Press:   [ID] 09 00 [modifier]   (single packet)
T=2.0s  → Hold:    [ID] 03 00 [modifier]   (single packet)
T=3.6s  → Release: [ID] 0A 00 00           (single packet)
```

This differs from motor commands which send continuous press packets every 50ms.

### GATT Handle Alignment

The chair caches GATT handles after bonding and expects the notify characteristic at a specific handle (`0x000D` observed). A **padding service** with a dummy characteristic and descriptors is inserted before the La-Z-Boy service to push the handles to the correct offsets. Without this, the chair bonds but never receives notifications.

## Troubleshooting

**Chair won't connect:**
- Verify the MAC address is correct (BLE MAC = base + 2)
- Factory reset the chair's pairing (unplug, hold Home on original remote, plug in)
- Run `wipe` command to clear ESP32 bonds, then power cycle the chair

**Chair connects but no response to commands:**
- Check Serial Monitor for "Ready for commands!" message
- If authentication fails, the handle alignment may be wrong — this sketch includes the padding service that works with tested chairs

**MQTT not connecting:**
- Verify WiFi credentials and MQTT broker address
- Check that the broker allows the connection (firewall, ACLs)

## Project Background

This project is the result of extensive BLE reverse engineering of the La-Z-Boy wireless remote protocol. Key discoveries:

- The chair acts as BLE **Central** (unusual — most BLE furniture accessories are peripherals)
- The remote acts as BLE **Peripheral**, advertising and waiting for the chair to connect
- Commands are sent via GATT **notifications** (not writes)
- The chair aggressively caches GATT handles after bonding, requiring exact handle alignment
- Save-preset uses a distinct 3-packet hold sequence, not continuous transmission

The BLE protocol analysis was done using nRF Connect, Wireshark with BLE sniffing, and Ghidra for firmware analysis.

## License

MIT License — see [LICENSE](LICENSE).
