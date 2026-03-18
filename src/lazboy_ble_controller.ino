/**
 * ╔═══════════════════════════════════════════════════════════════════════════╗
 * ║              LA-Z-BOY BLE WIRELESS REMOTE CONTROLLER                      ║
 * ║                         Version 2.0                                        ║
 * ╠═══════════════════════════════════════════════════════════════════════════╣
 * ║  Controls La-Z-Boy power recliners via BLE by emulating the wireless      ║
 * ║  remote. Supports Serial commands and MQTT for Home Assistant.            ║
 * ║                                                                           ║
 * ║  REQUIRES: Patched NimBLE-Arduino library (run setup.sh first!)           ║
 * ║                                                                           ║
 * ║  COMMANDS:                                                                ║
 * ║    Presets:  home, flat, tv              (tap to activate)                ║
 * ║    Save:     save_flat, save_tv          (3.5 sec hold per manual)        ║
 * ║    Motors:   head_up/down, recline_up/down, lumbar_up/down                ║
 * ║    Timed:    command:ms  (e.g., head_up:500 for half second)              ║
 * ║    System:   status, wipe, handles, help                                  ║
 * ║                                                                           ║
 * ║  SAVE PROTOCOL (exact timing from capture):                               ║
 * ║    Byte 2: 0x09 = Press, 0x03 = Hold, 0x0A = Release                     ║
 * ║    Sequence: Press(09) 2.0s -> Hold(03) 1.5s -> Release(0A) = 3.5s total ║
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
//                         HANDLE VERIFICATION
// =============================================================================
//
// The chair hardcodes GATT handle positions. After bonding, it expects:
//
//   Handle  | What
//   --------|-------------------------------
//   0x000B  | La-Z-Boy Service declaration
//   0x000C  | Notify char declaration
//   0x000D  | Notify VALUE              <-- critical
//   0x000E  | Notify CCCD               <-- critical
//   0x000F  | Write char declaration
//   0x0010  | Write VALUE
//   0x0011  | Bidir char declaration
//   0x0012  | Bidir VALUE
//   0x0013  | Bidir CCCD
//
// This requires the patched NimBLE library (run setup.sh) which removes
// the GATT Service Changed characteristic, freeing 3 handle slots.
//
// Without the patch: Notify VALUE lands at 0x0010 (too high, chair ignores)
// With the patch:    Notify VALUE lands at 0x000D (correct, chair responds)

#define TARGET_NOTIFY_VALUE  0x000D
#define TARGET_NOTIFY_CCCD   0x000E
#define TARGET_BIDIR_VALUE   0x0012
#define TARGET_BIDIR_CCCD    0x0013

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
    uint8_t hold[4];     // Hold packet (byte 2 = 0x03)
    uint8_t release[4];
    int holdMs;
    bool hasSaveMode;    // Does this command use the hold packet?
};

// Save sequence (from capture): press(09) 2.0s -> hold(03) 1.5s -> release(0A) = 3.5s total
Command commands[] = {
    // PRESETS - quick tap to activate
    {"home",         {0x93, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x93, 0x0A, 0x00, 0x00}, 200, false},
    {"flat",         {0x98, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x98, 0x0A, 0x00, 0x00}, 200, false},
    {"tv",           {0x94, 0x09, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x94, 0x0A, 0x00, 0x00}, 200, false},
    
    // SAVE PRESETS - 3-packet hold sequence
    {"save_flat",    {0x98, 0x09, 0x00, 0x01}, {0x98, 0x03, 0x00, 0x01}, {0x98, 0x0A, 0x00, 0x00}, 3500, true},
    {"save_tv",      {0x94, 0x09, 0x00, 0x02}, {0x94, 0x03, 0x00, 0x02}, {0x94, 0x0A, 0x00, 0x00}, 3500, true},
    
    // MOTORS - continuous press
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
//                         HANDLE DIAGNOSTIC
// =============================================================================

void printHandleDiagnostic() {
    uint16_t nv = notifyChar ? notifyChar->getHandle() : 0;
    uint16_t nc = notifyCCCD ? notifyCCCD->getHandle() : 0;
    uint16_t wv = writeChar  ? writeChar->getHandle()  : 0;
    uint16_t bv = bidirChar  ? bidirChar->getHandle()  : 0;
    uint16_t bc = bidirCCCD  ? bidirCCCD->getHandle()  : 0;
    
    bool notifyOk = (nv == TARGET_NOTIFY_VALUE);
    bool cccdOk   = (nc == TARGET_NOTIFY_CCCD);
    bool bidirOk  = (bv == TARGET_BIDIR_VALUE);
    bool bcccdOk  = (bc == TARGET_BIDIR_CCCD);
    bool allOk    = notifyOk && cccdOk && bidirOk && bcccdOk;
    
    Serial.println();
    Serial.println("============================================================");
    Serial.println("                  GATT HANDLE DIAGNOSTIC                     ");
    Serial.println("============================================================");
    Serial.printf( "  Notify VALUE:  0x%04X  (target 0x%04X)  %s\n",
                   nv, TARGET_NOTIFY_VALUE, notifyOk ? "OK" : "<-- WRONG");
    Serial.printf( "  Notify CCCD:   0x%04X  (target 0x%04X)  %s\n",
                   nc, TARGET_NOTIFY_CCCD,  cccdOk  ? "OK" : "<-- WRONG");
    Serial.printf( "  Write VALUE:   0x%04X\n", wv);
    Serial.printf( "  Bidir VALUE:   0x%04X  (target 0x%04X)  %s\n",
                   bv, TARGET_BIDIR_VALUE,  bidirOk ? "OK" : "<-- WRONG");
    Serial.printf( "  Bidir CCCD:    0x%04X  (target 0x%04X)  %s\n",
                   bc, TARGET_BIDIR_CCCD,   bcccdOk ? "OK" : "<-- WRONG");
    Serial.println("------------------------------------------------------------");
    
    if (allOk) {
        Serial.println("  >>> ALL HANDLES CORRECT <<<");
    } else if (nv == 0) {
        Serial.println("  Handles show 0x0000 - NimBLE may not report until");
        Serial.println("  after first connection. Type 'handles' after chair");
        Serial.println("  connects to re-check.");
    } else {
        int diff = (int)nv - (int)TARGET_NOTIFY_VALUE;
        Serial.printf( "  Notify VALUE is off by %+d handle(s)\n\n", diff);
        if (diff > 0) {
            Serial.println("  NimBLE patch may not be applied correctly.");
            Serial.println("  Run setup.sh or see patches/README for manual steps.");
        } else {
            Serial.println("  Unexpected: handles too low. Check NimBLE version.");
        }
    }
    Serial.println("============================================================");
    Serial.println();
}

// =============================================================================
//                         BLE FUNCTIONS
// =============================================================================

void enableNotifications() {
    uint8_t enabled[2] = {0x01, 0x00};
    if (notifyCCCD) notifyCCCD->setValue(enabled, 2);
    if (bidirCCCD) bidirCCCD->setValue(enabled, 2);
}

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        std::string value = pChar->getValue();
        Serial.printf("<- Chair wrote to %s: ", pChar->getUUID().toString().c_str());
        for (size_t i = 0; i < value.length() && i < 20; i++) {
            Serial.printf("%02X ", (uint8_t)value[i]);
        }
        Serial.println();
    }
    
    void onRead(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        Serial.printf("<- Chair read from %s\n", pChar->getUUID().toString().c_str());
    }
    
    void onSubscribe(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        Serial.printf("<- Chair subscribed to %s (value: %d)\n", 
                      pChar->getUUID().toString().c_str(), subValue);
    }
};

static CharCallbacks charCallbacks;

class BLECallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        chairConnected = true;
        connHandle = connInfo.getConnHandle();
        Serial.printf(">> Chair connected: %s\n", connInfo.getAddress().toString().c_str());
        NimBLEDevice::startSecurity(connHandle);
    }
    
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
        chairConnected = false;
        connHandle = 0;
        Serial.printf(">> Chair disconnected (reason: %d)\n", reason);
        delay(500);
        NimBLEDevice::startAdvertising();
    }
    
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf(">> Authenticated (bonded: %s)\n", connInfo.isBonded() ? "yes" : "no");
        enableNotifications();
        Serial.println(">> Ready for commands!\n");
    }
};

// =============================================================================
//                         COMMAND FUNCTIONS
// =============================================================================

void sendCommand(Command& cmd, int customHoldMs = -1) {
    if (!chairConnected) {
        Serial.println("!! Not connected to chair");
        return;
    }
    
    int holdMs = (customHoldMs > 0) ? customHoldMs : cmd.holdMs;
    
    enableNotifications();
    
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
        
        Serial.printf("-> %s (SAVE MODE)\n", cmd.name);
        
        Serial.println("  [T=0.0s] PRESS");
        notifyChar->setValue(pressPacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(pressPacket, 20);
        bidirChar->notify(connHandle);
        
        delay(2000);
        
        Serial.println("  [T=2.0s] HOLD");
        notifyChar->setValue(holdPacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(holdPacket, 20);
        bidirChar->notify(connHandle);
        
        delay(1600);
        
        Serial.println("  [T=3.6s] RELEASE");
        notifyChar->setValue(releasePacket, 20);
        notifyChar->notify(connHandle);
        bidirChar->setValue(releasePacket, 20);
        bidirChar->notify(connHandle);
        
        Serial.println(">> Done");
        return;
        
    } else {
        Serial.printf("-> %s (%d ms)\n", cmd.name, holdMs);
        
        unsigned long startTime = millis();
        while (millis() - startTime < (unsigned long)holdMs) {
            notifyChar->setValue(pressPacket, 20);
            notifyChar->notify(connHandle);
            bidirChar->setValue(pressPacket, 20);
            bidirChar->notify(connHandle);
            delay(PACKET_INTERVAL_MS);
        }
    }
    
    notifyChar->setValue(releasePacket, 20);
    notifyChar->notify(connHandle);
    bidirChar->setValue(releasePacket, 20);
    bidirChar->notify(connHandle);
    
    Serial.println(">> Done");
}

void executeCommand(const char* input) {
    String cmd = String(input);
    cmd.trim();
    cmd.toLowerCase();
    
    int colonIdx = cmd.indexOf(':');
    int customTime = -1;
    
    if (colonIdx > 0) {
        customTime = cmd.substring(colonIdx + 1).toInt();
        cmd = cmd.substring(0, colonIdx);
    }
    
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (cmd.equals(commands[i].name)) {
            sendCommand(commands[i], customTime);
            return;
        }
    }
    
    Serial.printf("!! Unknown command: %s\n", input);
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
        Serial.println(">> MQTT connected");
    }
}

// =============================================================================
//                         SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println();
    Serial.println("============================================");
    Serial.println("   LA-Z-BOY BLE CONTROLLER v2.0");
    Serial.println("============================================");
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
            Serial.printf("\n>> WiFi: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n!! WiFi failed (continuing offline)");
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
    
    // =========================================================================
    // LA-Z-BOY SERVICE
    //
    // No padding service needed - the NimBLE patch removes 3 handles from the
    // GATT service, which places our service at exactly the right offset.
    //
    // Handle layout (with patch applied):
    //   0x0001-0x0005: GAP service (Device Name + Appearance)
    //   0x0006-0x000A: GATT service (Srv Sup Feat + Client Sup Feat)
    //   0x000B:        LZB Service declaration
    //   0x000C:        Notify char declaration
    //   0x000D:        Notify VALUE          <-- chair reads here
    //   0x000E:        Notify CCCD           <-- chair subscribes here
    //   0x000F-0x0013: Write, Bidir, Read characteristics
    // =========================================================================
    NimBLEService* lzbService = bleServer->createService(SERVICE_UUID);
    
    // Notify characteristic (main command channel)
    notifyChar = lzbService->createCharacteristic(CHAR_NOTIFY_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    notifyChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    notifyChar->setCallbacks(&charCallbacks);
    notifyCCCD = notifyChar->createDescriptor("2902",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);
    notifyCCCD->setValue((uint8_t*)"\x00\x00", 2);
    
    // Write characteristic
    writeChar = lzbService->createCharacteristic(CHAR_WRITE_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE);
    writeChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    writeChar->setCallbacks(&charCallbacks);
    
    // Bidirectional characteristic
    bidirChar = lzbService->createCharacteristic(CHAR_BIDIR_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    bidirChar->setValue((uint8_t*)"\x00\x00\x00", 3);
    bidirChar->setCallbacks(&charCallbacks);
    bidirCCCD = bidirChar->createDescriptor("2902",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);
    bidirCCCD->setValue((uint8_t*)"\x00\x00", 2);
    
    // Read characteristic
    NimBLECharacteristic* readChar = lzbService->createCharacteristic(CHAR_READ_UUID, NIMBLE_PROPERTY::READ);
    readChar->setValue((uint8_t*)"\x00", 1);
    readChar->setCallbacks(&charCallbacks);
    
    lzbService->start();
    
    // --- Handle diagnostic ---
    printHandleDiagnostic();
    
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
    Serial.println("COMMANDS:");
    Serial.println("  Presets:  home, flat, tv");
    Serial.println("  Save:     save_flat, save_tv (3.5 sec hold)");
    Serial.println("  Head:     head_up, head_down");
    Serial.println("  Recline:  recline_up, recline_down (or feet_up/down)");
    Serial.println("  Lumbar:   lumbar_up, lumbar_down");
    Serial.println("  Timed:    command:ms (e.g., head_up:500)");
    Serial.println("  System:   status, wipe, handles, help");
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
    if (WiFi.status() == WL_CONNECTED && strlen(MQTT_SERVER) > 0) {
        if (!mqtt.connected()) connectMqtt();
        mqtt.loop();
    }
    
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
            Serial.println(">> Bonds cleared - re-pair required");
        }
        else if (input.equalsIgnoreCase("handles")) {
            printHandleDiagnostic();
        }
        else if (input.equalsIgnoreCase("help") || input.equals("?")) {
            Serial.println("COMMANDS:");
            Serial.println("  Presets:  home, flat, tv");
            Serial.println("  Save:     save_flat, save_tv (3.5 sec hold)");
            Serial.println("  Head:     head_up, head_down");
            Serial.println("  Recline:  recline_up, recline_down");
            Serial.println("  Lumbar:   lumbar_up, lumbar_down");
            Serial.println("  Timed:    command:ms (e.g., head_up:500)");
            Serial.println("  System:   status, wipe, handles, help");
        }
        else if (input.length() > 0) {
            executeCommand(input.c_str());
        }
    }
    
    delay(10);
}
