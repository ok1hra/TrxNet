// TrxNet basic example — ATMEGA2560 + Ethernet shield (W5500/W5100) (rotator)
//
// Publishes:  /freq  (uint32_t, Hz)        every 100 ms  — NON
//             /mode  (uint8_t, enum)        every 500 ms  — NON
//             /flags (uint16_t, bitfield)   every 1000 ms — NON
//             /cw    (char[], max 20 chars)  on demand     — CON
//
// Subscribes: same four paths — receives from any peer

#include <Ethernet.h>
#include <EthernetUDP.h>
#include <TrxNet.h>

// ---- device identity (set from EEPROM / config menu at runtime) ----
// Type and ID are kept separate so each can be configured independently.
// They are combined in setup() before net.begin() is called.
char deviceType[16] = "rotator";       // loaded from config/EEPROM
char deviceId[8]    = "01";            // loaded from config/EEPROM
char deviceName[TRXNET_MAX_DEVICE_NAME];  // assembled in setup()

// Change MAC to match the sticker on your Ethernet shield
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x02 };

// mode enum — must match the values used by all devices in the network
enum RadioMode : uint8_t { MODE_LSB = 0, MODE_USB, MODE_CW, MODE_RTTY, MODE_FM };

// flags bitfield — must match all devices
#define FLAG_PTT   (1 << 0)
#define FLAG_SPLIT (1 << 1)
#define FLAG_LOCK  (1 << 2)

// ---- TrxNet instance (name is not known yet at global init time) ----
EthernetUDP udp;
TrxNet      net(udp);

// ================================================================
// Receive callbacks
// Each callback receives:
//   from — sender's device name, e.g. "transceiver.01"
//   data — raw bytes (library does not interpret them)
//   len  — number of bytes
// ================================================================

void onFreq(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint32_t)) return;
    uint32_t freq;
    memcpy(&freq, data, sizeof(freq));
    // Use F() macro on AVR to keep strings in flash, not RAM
    Serial.print(F("["));
    Serial.print(from);
    Serial.print(F("] /freq  = "));
    Serial.print(freq);
    Serial.println(F(" Hz"));
}

void onMode(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint8_t)) return;
    uint8_t mode = data[0];
    Serial.print(F("["));
    Serial.print(from);
    Serial.print(F("] /mode  = "));
    Serial.println(mode);
}

void onFlags(const char* from, const uint8_t* data, size_t len) {
    if (len < sizeof(uint16_t)) return;
    uint16_t flags;
    memcpy(&flags, data, sizeof(flags));
    Serial.print(F("["));
    Serial.print(from);
    Serial.print(F("] /flags = 0x"));
    Serial.print(flags, HEX);
    Serial.print(F("  PTT="));
    Serial.print((flags & FLAG_PTT) ? 1 : 0);
    Serial.print(F(" SPLIT="));
    Serial.print((flags & FLAG_SPLIT) ? 1 : 0);
    Serial.print(F(" LOCK="));
    Serial.println((flags & FLAG_LOCK) ? 1 : 0);
}

void onCW(const char* from, const uint8_t* data, size_t len) {
    char msg[21] = {};
    memcpy(msg, data, (len < 20) ? len : 20);
    Serial.print(F("["));
    Serial.print(from);
    Serial.print(F("] /cw    = \""));
    Serial.print(msg);
    Serial.println('"');
}

// ================================================================
// setup / loop
// ================================================================

void setup() {
    Serial.begin(115200);

    if (Ethernet.begin(mac) == 0) {
        Serial.println(F("DHCP failed — check cable/shield"));
        while (true) {}
    }
    // Assemble full device name from type + id (replace with EEPROM read if needed)
    snprintf(deviceName, sizeof(deviceName), "%s.%s", deviceType, deviceId);

    Serial.print(F("IP: "));
    Serial.print(Ethernet.localIP());
    Serial.print(F("  name: "));
    Serial.println(deviceName);

    net.begin(deviceName);

    // Register receive handlers
    net.subscribe("/freq",  onFreq);
    net.subscribe("/mode",  onMode);
    net.subscribe("/flags", onFlags);
    net.subscribe("/cw",    onCW);
}

void loop() {
    // Required — call every loop iteration without blocking delays.
    Ethernet.maintain();
    net.loop();

    // ---- publish /freq every 100 ms (NON) ----
    static uint32_t tFreq = 0;
    if (millis() - tFreq >= 100) {
        uint32_t freq = 14250000UL;                  // replace with real value
        net.publish("/freq", (uint8_t*)&freq, sizeof(freq));
        tFreq = millis();
    }

    // ---- publish /mode every 500 ms (NON) ----
    static uint32_t tMode = 0;
    if (millis() - tMode >= 500) {
        uint8_t mode = MODE_CW;                      // replace with real value
        net.publish("/mode", &mode, sizeof(mode));
        tMode = millis();
    }

    // ---- publish /flags every 1000 ms (NON) ----
    static uint32_t tFlags = 0;
    if (millis() - tFlags >= 1000) {
        uint16_t flags = FLAG_PTT;                   // replace with real state
        net.publish("/flags", (uint8_t*)&flags, sizeof(flags));
        tFlags = millis();
    }

    // ---- publish /cw once a peer appears (CON — retransmits until ACKed) ----
    static bool cwSent = false;
    if (!cwSent && net.peerCount() > 0) {
        const char* msg = "73 DE OK1HRA";
        net.publish("/cw", (const uint8_t*)msg, strlen(msg), TRX_CON);
        cwSent = true;
    }
}
