/**
 * ╔═══════════════════════════════════════════════════════════════════════════╗
 * ║              LA-Z-BOY BLE WIRELESS REMOTE CONTROLLER                      ║
 * ║                         Version 1.2                                        ║
 * ╠═══════════════════════════════════════════════════════════════════════════╣
 * ║  Controls La-Z-Boy power recliners via BLE by emulating the wireless      ║
 * ║  remote. Supports Serial commands and MQTT for Home Assistant.            ║
 * ║                                                                           ║
 * ║  COMMANDS:                                                                ║
 * ║    Presets:  home, flat, tv              (tap to activate)                ║
 * ║    Save:     save_flat, save_tv          (3.5 sec hold per manual)          ║
 * ║    Motors:   head_up/down, recline_up/down, lumbar_up/down                ║
 * ║    Timed:    command:ms  (e.g., head_up:500 for half second)              ║
 * ║                                                                           ║
 * ║  SAVE PROTOCOL (exact timing from capture):                               ║
 * ║    Byte 2: 0x09 = Press, 0x03 = Hold, 0x0A = Release                      ║
 * ║    Sequence: Press(09) 2.0s → Hold(03) 1.5s → Release(0A) = 3.5s total    ║
 * ║                                                                           ║
 * ║  MQTT Topic: lazboy/command                                               ║
 * ╚═══════════════════════════════════════════════════════════════════════════╝
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>

extern "C" int esp_base_mac_addr_set(const uint8_t *mac);
extern "C" int esp_read_mac(uint8_t *mac, int type);
#define ESP_MAC_BT 2

// =============================================================================
//                         CONFIGURATION - EDIT THESE
// =============================================================================

// WiFi credentials (leave empty to skip WiFi)
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// MQTT broker settings (leave empty to skip MQTT)
const char* MQTT_SERVER   = "";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";
const char* MQTT_PASSWORD = "";
const char* MQTT_TOPIC    = "lazboy/command";

// Original remote's BLE MAC address
// IMPORTANT: ESP32 adds +2 to base MAC for BLE
// If your remote is XX:XX:XX:XX:XX:50, set last byte to 0x4E (0x50 - 2)
uint8_t REMOTE_MAC[6] = {0x00, 0x01, 0x90, 0x82, 0xB2, 0x4E};

// =============================================================================
//                         BLE PROTOCOL CONSTANTS
// =============================================================================

#define SERVICE_UUID        "826c364a-1005-11e8-b642-0ed5f89f718b"
#define CHAR_NOTIFY_UUID    "826c3d34-1005-11e8-b642-0ed5f89f718b"
#define CHAR_WRITE_UUID     "826c3fc8-1005-11e8-b642-0ed5f89f718b"
#define CHAR_BIDIR_UUID     "826c4248-1005-11e8-b642-0ed5f89f718b"
#define CHAR_READ_UUID      "826c44c8-1005-11e8-b642-0ed5f89f718b"

#define PACKET_INTERVAL_MS  50   // Send packets every 50ms while holding

// =============================================================================
//                         COMMAND DEFINITIONS
// =============================================================================

struct Command {
    const char* name;
    uint8_t press[4];
    uint8_t hold[4];     // NEW: Hold packet (byte 2 = 0x03)
    uint8_t release[4];
    int holdMs;
    bool hasSaveMode;    // NEW: Does this command use the hold packet?
};

// Commands with proper hold sequence for save functionality
// Save sequence (from capture): press(09) 2.0s → hold(03) 1.5s → release(0A) = 3.5s total
Command commands[] = {
    // PRESETS - 3-byte format (quick tap to activate)
    {"home",         {0x93, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x93, 0x0A, 0x00, 0x00}, 200, false},
    {"flat",         {0x98, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x98, 0x0A, 0x00, 0x00}, 200, false},
    {"tv",           {0x94, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x94, 0x0A, 0x00, 0x00}, 200, false},
    
    // SAVE PRESETS - 4-byte format with hold sequence from capture
    // Remote sends exactly 3 packets: press, hold at +2s, release at +3.6s
    {"save_flat",    {0x98, 0x09, 0x00, 0x01}, {0x98, 0x03, 0x00, 0x01}, {0x98, 0x0A, 0x00, 0x00}, 3500, true},
    {"save_tv",      {0x94, 0x09, 0x00, 0x02}, {0x94, 0x03, 0x00, 0x02}, {0x94, 0x0A, 0x00, 0x00}, 3500, true},
    
    // Motors - 3-byte format
    {"recline_up",   {0x11, 0x09, 0x02, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x11, 0x0A, 0x00, 0x00}, 1000, false},
    {"recline_down", {0x12, 0x09, 0x01, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x12, 0x0A, 0x00, 0x00}, 1000, false},
    {"feet_up",      {0x11, 0x09, 0x02, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x11, 0x0A, 0x00, 0x00}, 1000, false},
    {"feet_down",    {0x12, 0x09, 0x01, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x12, 0x0A, 0x00, 0x00}, 1000, false},
    {"head_up",      {0x21, 0x09, 0x40, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x21, 0x0A, 0x00, 0x00}, 1000, false},
    {"head_down",    {0x22, 0x09, 0x80, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x22, 0x0A, 0x00, 0x00}, 1000, false},
    {"lumbar_up",    {0x41, 0x09, 0x04, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x41, 0x0A, 0x00, 0x00}, 1000, false},
    {"lumbar_down",  {0x42, 0x09, 0x08, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x42, 0x0A, 0x00, 0x00}, 1000, false},
};

const int NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

// =============================================================================
//                         GLOBAL OBJECTS
// =============================================================================

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* notifyChar = nullptr;
NimBLECharacteristic* writeChar = nullptr;
NimBLECharacteristic* bidirChar = nullptr;
NimBLEDescriptor* notifyCCCD = nullptr;
NimBLEDescriptor* bidirCCCD = nullptr;

bool chairConnected = false;
uint16_t connHandle = 0;

// =============================================================================
//                         BLE FUNCTIONS
// =============================================================================

void enableNotifications() {
    uint8_t enabled[2] = {0x01, 0x00};
    if (notifyCCCD) notifyCCCD->setValue(enabled, 2);
    if (bidirCCCD) bidirCCCD->setValue(enabled, 2);
}

// Callback to capture what the chair writes to us
class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        std::string value = pChar->getValue();
        Serial.printf("← Chair wrote to %s: ", pChar->getUUID().toString().c_str());
        for (int i = 0; i < value.length() && i < 20; i++) {
            Serial.printf("%02X ", (uint8_t)value[i]);
        }
        Serial.println();
    }
    
    void onRead(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        Serial.printf("← Chair read from %s\n", pChar->getUUID().toString().c_str());
    }
    
    void onSubscribe(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        Serial.printf("← Chair subscribed to %s (value: %d)\n", 
                      pChar->getUUID().toString().c_str(), subValue);
    }
};

static CharCallbacks charCallbacks;

class BLECallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        chairConnected = true;
        connHandle = connInfo.getConnHandle();
        Serial.printf("✓ Chair connected: %s\n", connInfo.getAddress().toString().c_str());
        NimBLEDevice::startSecurity(connHandle);
    }
    
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
        chairConnected = false;
        connHandle = 0;
        Serial.printf("✗ Chair disconnected (reason: %d)\n", reason);
        delay(500);
        NimBLEDevice::startAdvertising();
    }
    
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf("✓ Authenticated (bonded: %s)\n", connInfo.isBonded() ? "yes" : "no");
        enableNotifications();
        Serial.println("✓ Ready for commands!\n");
    }
};

// =============================================================================
//                         COMMAND FUNCTIONS
// =============================================================================

void sendCommand(Command& cmd, int customHoldMs = -1) {
    if (!chairConnected) {
        Serial.println("✗ Not connected to chair");
        return;
    }
    
    int holdMs = (customHoldMs > 0) ? customHoldMs : cmd.holdMs;
    
    enableNotifications();
    
    // Build 20-byte packets
    uint8_t pressPacket[20] = {0};
    uint8_t holdPacket[20] = {0};
    uint8_t releasePacket[20] = {0};
    memcpy(pressPacket, cmd.press, 4);
    memcpy(holdPacket, cmd.hold, 4);
    memcpy(releasePacket, cmd.release, 4);
    
    if (cmd.hasSaveMode) {
        // SAVE SEQUENCE - exact behavior from capture:
        // Remote sends exactly 3 packets (NOT continuous):
        // 1. Press packet at T=0
        // 2. Hold packet at T=2.0s
        // 3. Release packet at T=3.6s
        
        Serial.printf("→ %s (SAVE MODE - single packets)\n", cmd.name);
        Serial.printf("  Press packet: %02X %02X %02X %02X\n", 
                      pressPacket[0], pressPacket[1], pressPacket[2], pressPacket[3]);
        Serial.printf("  Hold packet:  %02X %02X %02X %02X\n", 
                      holdPacket[0], holdPacket[1], holdPacket[2], holdPacket[3]);
        Serial.printf("  Release packet: %02X %02X %02X %02X\n", 
                      releasePacket[0], releasePacket[1], releasePacket[2], releasePacket[3]);
        
        // T=0: Send ONE press packet
        Serial.println("  [T=0.0s] Sending PRESS (09)...");
        notifyChar->setValue(pressPacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(pressPacket, 20);
        bidirChar->notify(connHandle);
        
        // Wait 2 seconds
        delay(2000);
        
        // T=2.0s: Send ONE hold packet
        Serial.println("  [T=2.0s] Sending HOLD (03)...");
        notifyChar->setValue(holdPacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(holdPacket, 20);
        bidirChar->notify(connHandle);
        
        // Wait 1.6 seconds (matching capture: 27.419 - 25.812 = 1.607s)
        delay(1600);
        
        // T=3.6s: Send ONE release packet
        Serial.println("  [T=3.6s] Sending RELEASE (0A)...");
        
        // Skip the release code at the end since we handle it here
        notifyChar->setValue(releasePacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(releasePacket, 20);
        bidirChar->notify(connHandle);
        
        Serial.println("✓ Done");
        return;  // Exit early, don't send release again
        
    } else {
        // NORMAL SEQUENCE: Just press for duration
        Serial.printf("→ %s (%d ms)\n", cmd.name, holdMs);
        
        unsigned long startTime = millis();
        while (millis() - startTime < (unsigned long)holdMs) {
            notifyChar->setValue(pressPacket, 20);
            notifyChar->notify(connHandle);
            bidirChar->setValue(pressPacket, 20);
            bidirChar->notify(connHandle);
            delay(PACKET_INTERVAL_MS);
        }
    }
    
    // Send release
    notifyChar->setValue(releasePacket, 20);
    notifyChar->notify(connHandle);
    bidirChar->setValue(releasePacket, 20);
    bidirChar->notify(connHandle);
    
    Serial.println("✓ Done");
}

void executeCommand(const char* input) {
    String cmd = String(input);
    cmd.trim();
    cmd.toLowerCase();
    
    // Check for timed command (e.g., "head_up:500")
    int colonIdx = cmd.indexOf(':');
    int customTime = -1;
    
    if (colonIdx > 0) {
        customTime = cmd.substring(colonIdx + 1).toInt();
        cmd = cmd.substring(0, colonIdx);
    }
    
    // Find and execute command
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (cmd.equals(commands[i].name)) {
            sendCommand(commands[i], customTime);
            return;
        }
    }
    
    Serial.printf("✗ Unknown command: %s\n", input);
}

// =============================================================================
//                         MQTT FUNCTIONS
// =============================================================================

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    char cmd[64] = {0};
    memcpy(cmd, payload, min((unsigned int)63, length));
    Serial.printf("MQTT: %s\n", cmd);
    executeCommand(cmd);
}

void connectMqtt() {
    if (mqtt.connected() || strlen(MQTT_SERVER) == 0) return;
    
    String clientId = "lazboy-" + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        mqtt.subscribe(MQTT_TOPIC);
        Serial.println("✓ MQTT connected");
    }
}

// =============================================================================
//                         SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println();
    Serial.println("╔═════════════════════════════════════════╗");
    Serial.println("║   LA-Z-BOY BLE CONTROLLER v1.2          ║");
    Serial.println("╚═════════════════════════════════════════╝");
    Serial.println();
    
    // --- WiFi ---
    if (strlen(WIFI_SSID) > 0) {
        Serial.print("Connecting to WiFi");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n✓ WiFi: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n✗ WiFi failed (continuing offline)");
        }
    }
    
    // --- MQTT ---
    if (strlen(MQTT_SERVER) > 0) {
        mqtt.setServer(MQTT_SERVER, MQTT_PORT);
        mqtt.setCallback(onMqttMessage);
    }
    
    // --- BLE MAC Address ---
    esp_base_mac_addr_set(REMOTE_MAC);
    
    // --- BLE Init ---
    NimBLEDevice::init("LZB-SHL");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    
    // Verify MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    Serial.printf("BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // --- BLE Server ---
    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new BLECallbacks());
    
    // Padding service (for GATT handle alignment)
    NimBLEService* padService = bleServer->createService("0000fee0-0000-1000-8000-00805f9b34fb");
    NimBLECharacteristic* padChar = padService->createCharacteristic(
        "0000fe00-0000-1000-8000-00805f9b34fb",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    padChar->setValue((uint8_t*)"\x00", 1);
    padChar->createDescriptor("2902", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2)
           ->setValue((uint8_t*)"\x00\x00", 2);
    padChar->createDescriptor("2901", NIMBLE_PROPERTY::READ, 8)
           ->setValue((uint8_t*)"Padding", 7);
    padService->start();
    
    // La-Z-Boy service
    NimBLEService* lzbService = bleServer->createService(SERVICE_UUID);
    
    notifyChar = lzbService->createCharacteristic(CHAR_NOTIFY_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    notifyChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    notifyChar->setCallbacks(&charCallbacks);
    notifyCCCD = notifyChar->createDescriptor("2902",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);
    notifyCCCD->setValue((uint8_t*)"\x00\x00", 2);
    
    writeChar = lzbService->createCharacteristic(CHAR_WRITE_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE);
    writeChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    writeChar->setCallbacks(&charCallbacks);
    
    bidirChar = lzbService->createCharacteristic(CHAR_BIDIR_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    bidirChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    bidirChar->setCallbacks(&charCallbacks);
    bidirCCCD = bidirChar->createDescriptor("2902",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);
    bidirCCCD->setValue((uint8_t*)"\x00\x00", 2);
    
    NimBLECharacteristic* readChar = lzbService->createCharacteristic(CHAR_READ_UUID, NIMBLE_PROPERTY::READ);
    readChar->setValue((uint8_t*)"\x00", 1);
    readChar->setCallbacks(&charCallbacks);
    
    lzbService->start();
    
    // --- Advertising ---
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(0x05);
    advData.setManufacturerData(std::string("\x65\x02\x03\x01", 4));
    advData.setName("LZB-SHL");
    adv->setAdvertisementData(advData);
    adv->addServiceUUID(SERVICE_UUID);
    adv->start();
    
    // --- Print Help ---
    Serial.println();
    Serial.println("COMMANDS:");
    Serial.println("  Presets:  home, flat, tv");
    Serial.println("  Save:     save_flat, save_tv (3.5 sec hold)");
    Serial.println("  Head:     head_up, head_down");
    Serial.println("  Recline:  recline_up, recline_down (or feet_up/down)");
    Serial.println("  Lumbar:   lumbar_up, lumbar_down");
    Serial.println("  Timed:    command:ms (e.g., head_up:500)");
    Serial.println("  System:   status, wipe, help");
    Serial.println();
    Serial.printf("MQTT Topic: %s\n", MQTT_TOPIC);
    Serial.println();
    Serial.println("Waiting for chair to connect...");
    Serial.println();
}

// =============================================================================
//                         MAIN LOOP
// =============================================================================

void loop() {
    // MQTT maintenance
    if (WiFi.status() == WL_CONNECTED && strlen(MQTT_SERVER) > 0) {
        if (!mqtt.connected()) connectMqtt();
        mqtt.loop();
    }
    
    // Serial commands
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        
        if (input.equalsIgnoreCase("status")) {
            Serial.printf("Chair: %s\n", chairConnected ? "Connected" : "Waiting");
            Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Offline");
            Serial.printf("MQTT: %s\n", mqtt.connected() ? "Connected" : "Offline");
            Serial.printf("Bonds: %d\n", NimBLEDevice::getNumBonds());
        }
        else if (input.equalsIgnoreCase("wipe")) {
            NimBLEDevice::deleteAllBonds();
            Serial.println("✓ Bonds cleared - re-pair required");
        }
        else if (input.equalsIgnoreCase("help") || input.equals("?")) {
            Serial.println("COMMANDS:");
            Serial.println("  Presets:  home, flat, tv");
            Serial.println("  Save:     save_flat, save_tv (3.5 sec hold)");
            Serial.println("  Head:     head_up, head_down");
            Serial.println("  Recline:  recline_up, recline_down");
            Serial.println("  Lumbar:   lumbar_up, lumbar_down");
            Serial.println("  Timed:    command:ms (e.g., head_up:500)");
            Serial.println("  System:   status, wipe, help");
        }
        else if (input.length() > 0) {
            executeCommand(input.c_str());
        }
    }
    
    delay(10);
}
