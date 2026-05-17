// TrxNet basic example — ESP32 (transceiver)
//
// Publishes:  /freq  (uint32_t, Hz)        every 100 ms  — NON
//             /mode  (uint8_t, enum)        every 500 ms  — NON
//             /flags (uint16_t, bitfield)   every 1000 ms — NON
//             /cw    (char[], max 20 chars)  on demand     — CON
//
// Subscribes: same four paths — receives from any peer

#include <WiFi.h>
#include <WiFiUDP.h>
#include <TrxNet.h>

// ---- network credentials ----
#define WIFI_SSID  "YOUR_SSID"
#define WIFI_PASS  "YOUR_PASS"

// ---- device identity (set from EEPROM / config menu at runtime) ----
// Type and ID are kept separate so each can be configured independently.
// They are combined in setup() before net.begin() is called.
char deviceType[16] = "transceiver";   // loaded from config/EEPROM
char deviceId[8]    = "01";            // loaded from config/EEPROM
char deviceName[TRXNET_MAX_DEVICE_NAME];  // assembled in setup()

// mode enum shared between all devices (agree on values in your project)
enum RadioMode : uint8_t { MODE_LSB = 0, MODE_USB, MODE_CW, MODE_RTTY, MODE_FM };

// flags bitfield
#define FLAG_PTT   (1 << 0)   // bit 0 — transmitting
#define FLAG_SPLIT (1 << 1)   // bit 1 — split VFO active
#define FLAG_LOCK  (1 << 2)   // bit 2 — VFO locked

// ---- TrxNet instance (name is not known yet at global init time) ----
WiFiUDP udp;
TrxNet  net(udp);

// ================================================================
// Receive callbacks
// Each callback receives:
//   from — sender's device name, e.g. "rotator.01"
//   data — raw bytes (library does not interpret them)
//   len  — number of bytes
// ================================================================

void onFreq(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint32_t)) return;
    uint32_t freq;
    memcpy(&freq, data, sizeof(freq));
    Serial.printf("[%s] /freq  = %lu Hz\n", from, (unsigned long)freq);
}

void onMode(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint8_t)) return;
    uint8_t mode = data[0];
    const char* names[] = { "LSB", "USB", "CW", "RTTY", "FM" };
    const char* label = (mode < 5) ? names[mode] : "?";
    Serial.printf("[%s] /mode  = %u (%s)\n", from, mode, label);
}

void onFlags(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint16_t)) return;
    uint16_t flags;
    memcpy(&flags, data, sizeof(flags));
    Serial.printf("[%s] /flags = 0x%04X  PTT=%d SPLIT=%d LOCK=%d\n",
        from, flags,
        (flags & FLAG_PTT)   ? 1 : 0,
        (flags & FLAG_SPLIT) ? 1 : 0,
        (flags & FLAG_LOCK)  ? 1 : 0);
}

void onCW(const char* from, const uint8_t* data, size_t len) {
    char msg[21] = {};
    memcpy(msg, data, (len < 20) ? len : 20);
    Serial.printf("[%s] /cw    = \"%s\"\n", from, msg);
}

// ================================================================
// setup / loop
// ================================================================

void setup() {
    Serial.begin(115200);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    // Assemble full device name from type + id (replace with EEPROM read if needed)
    snprintf(deviceName, sizeof(deviceName), "%s.%s", deviceType, deviceId);

    Serial.printf("\nIP: %s  name: %s\n",
        WiFi.localIP().toString().c_str(), deviceName);

    net.begin(deviceName);

    // Register receive handlers
    net.subscribe("/freq",  onFreq);
    net.subscribe("/mode",  onMode);
    net.subscribe("/flags", onFlags);
    net.subscribe("/cw",    onCW);
}

void loop() {
    // Must be called every loop iteration — processes incoming packets,
    // sends keepalive, retransmits unACKed CON messages.
    net.loop();

    // ---- publish /freq every 100 ms (NON) ----
    static uint32_t tFreq = 0;
    if (millis() - tFreq >= 100) {
        uint32_t freq = 14250000UL;                  // replace with real VFO read
        net.publish("/freq", (uint8_t*)&freq, sizeof(freq));
        tFreq = millis();
    }

    // ---- publish /mode every 500 ms (NON) ----
    static uint32_t tMode = 0;
    if (millis() - tMode >= 500) {
        uint8_t mode = MODE_USB;                     // replace with real mode read
        net.publish("/mode", &mode, sizeof(mode));
        tMode = millis();
    }

    // ---- publish /flags every 1000 ms (NON) ----
    static uint32_t tFlags = 0;
    if (millis() - tFlags >= 1000) {
        uint16_t flags = FLAG_SPLIT;                 // replace with real state
        net.publish("/flags", (uint8_t*)&flags, sizeof(flags));
        tFlags = millis();
    }

    // ---- publish /cw once a peer appears (CON — retransmits until ACKed) ----
    static bool cwSent = false;
    if (!cwSent && net.peerCount() > 0) {
        const char* msg = "CQ CQ DE OK1HRA";
        net.publish("/cw", (const uint8_t*)msg, strlen(msg), TRX_CON);
        cwSent = true;
    }
}
