# La-Z-Boy BLE Wireless Remote Controller

Control La-Z-Boy power recliners via BLE using an ESP32, with optional MQTT integration for Home Assistant.

The ESP32 emulates the La-Z-Boy wireless remote by acting as a BLE peripheral. The chair (which acts as BLE central) connects to the ESP32 and accepts motor/preset commands over GATT notifications — exactly as it would from the original remote.

## Quick Start

```bash
# 1. Clone
git clone https://github.com/YOUR_USER/lazboy-ble-controller.git
cd lazboy-ble-controller

# 2. Install libraries in Arduino IDE:
#    - NimBLE-Arduino by h2zero (search "NimBLE" in Library Manager)
#    - PubSubClient by Nick O'Leary

# 3. Patch NimBLE (required — see why below)
chmod +x setup.sh
./setup.sh

# 4. Edit src/lazboy_ble_controller.ino:
#    - Set REMOTE_MAC to your remote's MAC (subtract 2 from last byte)
#    - Optionally set WiFi + MQTT credentials

# 5. Flash and open Serial Monitor at 115200

# 6. Factory reset the chair pairing:
#    - Unplug chair, hold Home on original remote, plug in, release after 5s

# 7. Chair connects, type: head_up
```

## How It Works

La-Z-Boy power recliners use a BLE architecture where the **chair is the central** and the **remote is the peripheral**. This is the reverse of most BLE setups. The remote advertises, the chair connects, bonds, and then listens for GATT notifications on specific characteristics.

This firmware:

1. Spoofs the original remote's BLE MAC address
2. Advertises with the same service UUID and manufacturer data
3. Creates a GATT server with the La-Z-Boy service and characteristics
4. Accepts the chair's connection and completes bonding
5. Sends command packets as notifications on two characteristics simultaneously

Commands are 3-4 byte sequences where byte 2 encodes the action: `0x09` = press, `0x03` = hold, `0x0A` = release.

## Hardware Required

- **ESP32** development board (ESP32-WROOM or ESP32-DevKitC)
- USB cable for flashing and serial monitor

No additional components needed.

## Software Dependencies

| Library | Version Tested | Purpose |
|---------|---------------|---------|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | 2.3.9 | BLE stack (requires patch, see below) |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | 2.8+ | Optional MQTT client |
| ESP32 Arduino Core | 3.3.7 | Board support |

## NimBLE Patch (Required)

La-Z-Boy chairs **hardcode GATT handle positions** in their firmware. The chair expects the notify characteristic at handle `0x000D`. If it's anywhere else, the chair connects but silently ignores all commands.

NimBLE's built-in GATT Service Changed characteristic consumes 3 handle slots that push our service to the wrong position. The patch comments out this one characteristic — a 6-line change in one file.

```bash
# Apply automatically:
./setup.sh

# Or manually: see patches/README.md
```

The Service Changed characteristic tells BLE clients when the GATT table changes at runtime. Since our table is static, removing it has zero functional impact. See [patches/README.md](patches/README.md) for the full technical explanation and handle layout.

After the patch, type `handles` in Serial Monitor to verify all handles show `OK`.

## Setup

### 1. Find Your Remote's MAC Address

Use a BLE scanner app (nRF Connect) to find a device advertising as `LZB-SHL`.

**Important:** The ESP32 adds +2 to the base MAC for BLE. If your remote's BLE MAC ends in `0x50`, set the last byte in `REMOTE_MAC` to `0x4E` (i.e., `0x50 - 2`).

### 2. Unpair the Original Remote

The chair bonds with one remote at a time. Factory reset the chair's pairing:

1. Unplug the chair from power
2. Hold the Home button on the original remote
3. Plug the chair back in while still holding Home
4. Release after ~5 seconds

### 3. Configure and Flash

Edit the configuration section in `src/lazboy_ble_controller.ino`, flash, and open Serial Monitor at 115200 baud.

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
| `handles` | Print GATT handle diagnostic |
| `help` | Show command list |

### Timed Commands

Append `:ms` to any motor command to run it for a specific duration:

```
head_up:500       # Half second
recline_down:3000 # Three seconds
lumbar_up:250     # Quarter second
```

## Home Assistant Integration

### MQTT

With WiFi and MQTT configured, control the chair from Home Assistant:

```yaml
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
| `826c364a-...` | Primary Service | - |
| `826c3d34-...` | Notify | Read, Notify |
| `826c3fc8-...` | Write | Write, Write No Response |
| `826c4248-...` | Bidirectional | Write, Write No Response, Notify |
| `826c44c8-...` | Read | Read |

### Command Packet Format

Commands are 20-byte packets (zero-padded). The first 3-4 bytes carry the command:

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
T=0.0s  -> Press:   [ID] 09 00 [modifier]   (single packet)
T=2.0s  -> Hold:    [ID] 03 00 [modifier]   (single packet)
T=3.6s  -> Release: [ID] 0A 00 00           (single packet)
```

This differs from motor commands which send continuous press packets every 50ms.

## Troubleshooting

**Chair won't connect:**
- Verify the MAC address is correct (BLE MAC = base + 2)
- Factory reset the chair's pairing
- Run `wipe` command to clear ESP32 bonds, then power cycle the chair

**Chair connects but commands are ignored:**
- Type `handles` — if Notify VALUE isn't `0x000D`, the NimBLE patch isn't applied correctly
- Run `./setup.sh` again or see `patches/README.md` for manual steps
- Make sure you ran `wipe` and factory reset the chair after any firmware change

**MQTT not connecting:**
- Verify WiFi credentials and MQTT broker address
- Check that the broker allows the connection

**Handles show 0x0000:**
- Normal for some NimBLE versions at boot — type `handles` after the chair connects

## Project Background

This project is the result of extensive BLE reverse engineering of the La-Z-Boy wireless remote protocol. Key discoveries:

- The chair acts as BLE **central** (unusual for furniture)
- The remote acts as BLE **peripheral**, advertising for the chair to connect
- Commands are sent via GATT **notifications** (not writes)
- The chair **hardcodes GATT handle positions** and never re-discovers after bonding
- Save-preset uses a distinct 3-packet hold sequence, not continuous transmission
- NimBLE's Service Changed characteristic must be removed to achieve correct handle alignment

The BLE protocol was analyzed using nRF Connect, HCI snoop logs, and Wireshark.

## License

MIT License — see [LICENSE](LICENSE).
