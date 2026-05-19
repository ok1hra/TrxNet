#include "TrxNet.h"
#include <string.h>

// ---- discovery packet constants ----
// First byte 0xAA has version bits 10 — invalid CoAP, safe to distinguish.
#define DISC_MAGIC     0xAA
#define DISC_VERSION   0x01
#define DISC_PROBE     0x01   // broadcast "who is there?"
#define DISC_ANNOUNCE  0x02   // broadcast or unicast "I am here"

// ---- CoAP constants ----
#define COAP_VER       1
#define COAP_CON       0
#define COAP_NON       1
#define COAP_ACK       2
#define COAP_POST      0x02   // code 0.02
#define COAP_EMPTY     0x00   // code 0.00  (empty ACK)
#define COAP_URI_PATH  11     // option number

// ========================================================================
// Constructor
// ========================================================================
TrxNet::TrxNet(UDP& udp, uint16_t port)
    : _udp(udp), _port(port), _lastAnnounce(0), _msgId(1), _seenIdx(0)
{
    _name[0] = '\0';
    memset(_peers,   0, sizeof(_peers));
    memset(_subs,    0, sizeof(_subs));
    memset(_pending, 0, sizeof(_pending));
    memset(_seen,    0, sizeof(_seen));
}

// ========================================================================
// Public API
// ========================================================================
void TrxNet::begin(const char* name) {
    strncpy(_name, name, TRXNET_MAX_DEVICE_NAME - 1);
    _name[TRXNET_MAX_DEVICE_NAME - 1] = '\0';
    _udp.begin(_port);
    _sendDiscovery(DISC_PROBE, IPAddress(255, 255, 255, 255));
    _lastAnnounce = millis();
}

void TrxNet::loop() {
    _handleIncoming();
    _checkTimeouts();

    if (millis() - _lastAnnounce >= TRXNET_ANNOUNCE_MS) {
        _sendDiscovery(DISC_ANNOUNCE, IPAddress(255, 255, 255, 255));
        _lastAnnounce = millis();
    }
}

void TrxNet::subscribe(const char* path, TrxNetCallback cb) {
    if (strlen(path) >= TRXNET_MAX_TOPIC_LEN) return;  // path too long to store or match
    for (int i = 0; i < TRXNET_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].path, path) == 0) {
            _subs[i].cb = cb;
            return;
        }
    }
    for (int i = 0; i < TRXNET_MAX_SUBS; i++) {
        if (!_subs[i].active) {
            strncpy(_subs[i].path, path, TRXNET_MAX_TOPIC_LEN - 1);
            _subs[i].path[TRXNET_MAX_TOPIC_LEN - 1] = '\0';
            _subs[i].cb     = cb;
            _subs[i].active = true;
            return;
        }
    }
}

void TrxNet::unsubscribe(const char* path) {
    for (int i = 0; i < TRXNET_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].path, path) == 0) {
            _subs[i].active = false;
            return;
        }
    }
}

void TrxNet::publish(const char* path, const uint8_t* data, size_t len,
                     TrxMsgType type)
{
    if (strlen(path) >= TRXNET_MAX_TOPIC_LEN) return;  // path too long — would encode a truncated, undeliverable topic
    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (!_peers[i].active) continue;

        uint16_t id = _nextMsgId();

        if (type == TRX_CON) {
            for (int j = 0; j < TRXNET_MAX_PENDING; j++) {
                if (_pending[j].active) continue;
                _pending[j].len     = _buildCoAP(_pending[j].buf, path,
                                                  data, len, type, id);
                _pending[j].ip      = _peers[i].ip;
                _pending[j].port    = _peers[i].port;
                _pending[j].msgId   = id;
                _pending[j].sentAt  = millis();
                _pending[j].retries = 0;
                _pending[j].active  = true;
                _sendRaw(_peers[i].ip, _peers[i].port,
                         _pending[j].buf, _pending[j].len);
                break;
            }
        } else {
            uint8_t buf[TRXNET_PKT_MAX];
            size_t  pktLen = _buildCoAP(buf, path, data, len, type, id);
            _sendRaw(_peers[i].ip, _peers[i].port, buf, pktLen);
        }
    }
}

void TrxNet::setPort(uint16_t port) {
    _port = port;
}

int TrxNet::peerCount() const {
    int n = 0;
    for (int i = 0; i < TRXNET_MAX_PEERS; i++)
        if (_peers[i].active) n++;
    return n;
}

const TrxPeer* TrxNet::peer(int index) const {
    int n = 0;
    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (_peers[i].active) {
            if (n == index) return &_peers[i];
            n++;
        }
    }
    return NULL;
}

// ========================================================================
// Private — discovery
// ========================================================================

// Discovery packet layout:
//   [0]     DISC_MAGIC (0xAA)
//   [1]     DISC_VERSION (0x01)
//   [2]     packet type (DISC_PROBE / DISC_ANNOUNCE)
//   [3]     name length
//   [4..N]  device name (no null terminator)
//   [N+1]   port high byte
//   [N+2]   port low byte
void TrxNet::_sendDiscovery(uint8_t pktType, IPAddress dest) {
    uint8_t buf[4 + TRXNET_MAX_DEVICE_NAME + 2];
    uint8_t nameLen = (uint8_t)strlen(_name);
    size_t  pos = 0;

    buf[pos++] = DISC_MAGIC;
    buf[pos++] = DISC_VERSION;
    buf[pos++] = pktType;
    buf[pos++] = nameLen;
    memcpy(&buf[pos], _name, nameLen);
    pos += nameLen;
    buf[pos++] = (_port >> 8) & 0xFF;
    buf[pos++] = _port & 0xFF;

    _udp.beginPacket(dest, _port);
    _udp.write(buf, pos);
    _udp.endPacket();
}

void TrxNet::_processDiscovery(IPAddress src, const uint8_t* buf, size_t len) {
    if (len < 4 || buf[1] != DISC_VERSION) return;

    uint8_t nameLen = buf[3];
    if (len < (size_t)(4 + nameLen + 2)) return;

    char    name[TRXNET_MAX_DEVICE_NAME];
    uint8_t copyLen = (nameLen < TRXNET_MAX_DEVICE_NAME - 1)
                          ? nameLen
                          : (uint8_t)(TRXNET_MAX_DEVICE_NAME - 1);
    memcpy(name, &buf[4], copyLen);
    name[copyLen] = '\0';

    if (strcmp(name, _name) == 0) return;

    uint16_t port = ((uint16_t)buf[4 + nameLen] << 8) | buf[4 + nameLen + 1];
    _findOrAddPeer(name, src, port);

    if (buf[2] == DISC_PROBE) {
        _sendDiscovery(DISC_ANNOUNCE, src);
    }
}

// ========================================================================
// Private — CoAP
// ========================================================================

// CoAP packet layout (RFC 7252):
//   Byte 0: VV TT LLLL  (ver=01, type, token-length)
//   Byte 1: Code
//   Byte 2-3: Message ID
//   [Token: TKL bytes — we always use TKL=0]
//   [Options: delta-length encoded]
//   [0xFF payload marker]
//   [Payload]

size_t TrxNet::_buildCoAP(uint8_t* buf, const char* path,
                            const uint8_t* data, size_t dataLen,
                            TrxMsgType type, uint16_t msgId)
{
    size_t  pos      = 0;
    uint8_t coapType = (type == TRX_CON) ? COAP_CON : COAP_NON;

    // Header (TKL=0) — always 4 bytes, buffer is guaranteed >= TRXNET_PKT_MAX > 4
    buf[pos++] = (uint8_t)((COAP_VER << 6) | (coapType << 4));
    buf[pos++] = COAP_POST;
    buf[pos++] = (msgId >> 8) & 0xFF;
    buf[pos++] = msgId & 0xFF;

    // Uri-Path options — split path on '/', encode each segment separately
    const char* p = path;
    if (*p == '/') p++;

    uint8_t prevOpt = 0;
    while (*p) {
        const char* segEnd = strchr(p, '/');
        uint8_t segLen = segEnd
                             ? (uint8_t)(segEnd - p)
                             : (uint8_t)strlen(p);

        if (segLen == 0) {
            if (segEnd) { p = segEnd + 1; }
            else          break;
            continue;
        }

        uint8_t delta = COAP_URI_PATH - prevOpt;
        prevOpt       = COAP_URI_PATH;

        uint8_t dNib = (delta  < 13) ? delta  : 13;
        uint8_t lNib = (segLen < 13) ? segLen : 13;

        // Calculate bytes needed: header byte + optional ext bytes + segment data
        size_t needed = 1
                        + (delta  >= 13 ? 1 : 0)
                        + (segLen >= 13 ? 1 : 0)
                        + segLen;
        if (pos + needed > TRXNET_PKT_MAX) break;  // path too long — stop encoding

        buf[pos++] = (uint8_t)((dNib << 4) | lNib);
        if (delta  >= 13) buf[pos++] = (uint8_t)(delta  - 13);
        if (segLen >= 13) buf[pos++] = (uint8_t)(segLen - 13);

        memcpy(&buf[pos], p, segLen);
        pos += segLen;

        p = segEnd ? segEnd + 1 : p + segLen;
    }

    // Payload — only write if there is room for marker + at least 1 byte
    if (dataLen > 0 && pos + 2 <= TRXNET_PKT_MAX) {
        size_t room    = TRXNET_PKT_MAX - pos - 1;  // subtract marker byte
        size_t copyLen = dataLen < TRXNET_MAX_PAYLOAD ? dataLen : TRXNET_MAX_PAYLOAD;
        if (copyLen > room) copyLen = room;
        buf[pos++] = 0xFF;  // payload marker
        memcpy(&buf[pos], data, copyLen);
        pos += copyLen;
    }

    return pos;
}

void TrxNet::_sendACK(IPAddress ip, uint16_t port, uint16_t msgId) {
    uint8_t buf[4];
    buf[0] = (uint8_t)((COAP_VER << 6) | (COAP_ACK << 4));  // TKL=0
    buf[1] = COAP_EMPTY;
    buf[2] = (msgId >> 8) & 0xFF;
    buf[3] = msgId & 0xFF;
    _sendRaw(ip, port, buf, 4);
}

// ========================================================================
// Private — incoming packet dispatch
// ========================================================================
void TrxNet::_handleIncoming() {
    int pktSize;
    while ((pktSize = _udp.parsePacket()) > 0) {
        uint8_t   buf[256];
        size_t    len     = (size_t)_udp.read(buf, (int)sizeof(buf));
        IPAddress src     = _udp.remoteIP();
        uint16_t  srcPort = _udp.remotePort();

        if (len < 1) continue;

        if (buf[0] == DISC_MAGIC) {
            _processDiscovery(src, buf, len);
        } else if (((buf[0] >> 6) & 0x03) == COAP_VER) {
            _processCoAP(src, srcPort, buf, len);
        }
    }
}

void TrxNet::_processCoAP(IPAddress src, uint16_t srcPort,
                            const uint8_t* buf, size_t len)
{
    if (len < 4) return;

    uint8_t  msgType = (buf[0] >> 4) & 0x03;
    uint8_t  tkl     = buf[0] & 0x0F;
    uint8_t  code    = buf[1];
    uint16_t msgId   = ((uint16_t)buf[2] << 8) | buf[3];

    // ACK — clear matching pending entry
    if (msgType == COAP_ACK) {
        for (int i = 0; i < TRXNET_MAX_PENDING; i++) {
            if (_pending[i].active &&
                _pending[i].msgId == msgId &&
                _pending[i].ip    == src)
            {
                _pending[i].active = false;
            }
        }
        return;
    }

    if (code != COAP_POST) return;

    // Send ACK immediately for CON — before any further processing.
    // This ensures the ACK goes out even if parsing or dispatch takes time,
    // and prevents the sender from retransmitting unnecessarily.
    if (msgType == COAP_CON)
        _sendACK(src, srcPort, msgId);

    // CON deduplication: if this (src, msgId) was already dispatched,
    // the earlier ACK was lost in transit and the sender retransmitted.
    // Drop the duplicate to prevent the callback firing more than once.
    if (msgType == COAP_CON) {
        for (int i = 0; i < TRXNET_MAX_SEEN; i++) {
            if (_seen[i].msgId == msgId && _seen[i].ip == src)
                return;
        }
        // Record as seen before dispatching
        _seen[_seenIdx].ip    = src;
        _seen[_seenIdx].msgId = msgId;
        _seenIdx = (_seenIdx + 1) % TRXNET_MAX_SEEN;
    }

    // Skip token
    size_t pos = 4 + tkl;
    if (pos > len) return;

    // Parse Uri-Path options into a path string.
    // Track whether the path was truncated — a truncated path must not be
    // dispatched, as it could accidentally match a registered subscription.
    char    path[TRXNET_MAX_TOPIC_LEN];
    path[0]         = '/';
    size_t  pathLen  = 1;
    bool    truncated = false;
    uint16_t optNum  = 0;

    while (pos < len && buf[pos] != 0xFF) {
        if (pos + 1 > len) return;

        uint16_t d = (buf[pos] >> 4) & 0x0F;
        uint16_t l = buf[pos] & 0x0F;
        pos++;

        if      (d == 13) { if (pos >= len) return; d = buf[pos++] + 13; }
        else if (d == 14) { if (pos + 1 >= len) return;
                             d = (((uint16_t)buf[pos] << 8) | buf[pos+1]) + 269;
                             pos += 2; }

        if      (l == 13) { if (pos >= len) return; l = buf[pos++] + 13; }
        else if (l == 14) { if (pos + 1 >= len) return;
                             l = (((uint16_t)buf[pos] << 8) | buf[pos+1]) + 269;
                             pos += 2; }

        optNum += d;

        if (optNum == COAP_URI_PATH && l > 0) {
            if (pathLen > 1 && pathLen < TRXNET_MAX_TOPIC_LEN - 1)
                path[pathLen++] = '/';
            if (pathLen + l >= TRXNET_MAX_TOPIC_LEN) {
                truncated = true;  // path exceeds buffer — mark and stop parsing
            } else {
                if (pos + l > len) return;
                memcpy(&path[pathLen], &buf[pos], l);
                pathLen += l;
            }
        }

        if (pos + l > len) return;
        pos += l;
    }
    path[pathLen] = '\0';

    // A truncated path cannot be matched reliably — discard silently.
    if (truncated) return;

    // Skip payload marker
    if (pos < len && buf[pos] == 0xFF) pos++;

    const uint8_t* payload    = &buf[pos];
    size_t         payloadLen = (pos <= len) ? (len - pos) : 0;

    // Resolve sender name from peer list
    char fromName[TRXNET_MAX_DEVICE_NAME];
    fromName[0] = '\0';
    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (_peers[i].active && _peers[i].ip == src) {
            strncpy(fromName, _peers[i].name, sizeof(fromName) - 1);
            fromName[sizeof(fromName) - 1] = '\0';
            break;
        }
    }

    // Dispatch to all matching subscriptions
    for (int i = 0; i < TRXNET_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].path, path) == 0)
            _subs[i].cb(fromName, payload, payloadLen);
    }
}

// ========================================================================
// Private — helpers
// ========================================================================
void TrxNet::_sendRaw(IPAddress ip, uint16_t port,
                       const uint8_t* buf, size_t len)
{
    _udp.beginPacket(ip, port);
    _udp.write(buf, len);
    _udp.endPacket();
}

void TrxNet::_checkTimeouts() {
    uint32_t now = millis();

    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (_peers[i].active &&
            (now - _peers[i].lastSeen) > TRXNET_PEER_TIMEOUT_MS)
        {
            _peers[i].active = false;
        }
    }

    for (int i = 0; i < TRXNET_MAX_PENDING; i++) {
        if (!_pending[i].active) continue;
        if ((now - _pending[i].sentAt) < TRXNET_CON_TIMEOUT_MS) continue;

        if (_pending[i].retries >= TRXNET_CON_MAX_RETRIES) {
            _pending[i].active = false;
        } else {
            _pending[i].retries++;
            _pending[i].sentAt = now;
            _sendRaw(_pending[i].ip, _pending[i].port,
                     _pending[i].buf, _pending[i].len);
        }
    }
}

TrxPeer* TrxNet::_findOrAddPeer(const char* name, IPAddress ip, uint16_t port) {
    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (_peers[i].active && strcmp(_peers[i].name, name) == 0) {
            _peers[i].ip       = ip;
            _peers[i].port     = port;
            _peers[i].lastSeen = millis();
            return &_peers[i];
        }
    }
    for (int i = 0; i < TRXNET_MAX_PEERS; i++) {
        if (!_peers[i].active) {
            strncpy(_peers[i].name, name, TRXNET_MAX_DEVICE_NAME - 1);
            _peers[i].name[TRXNET_MAX_DEVICE_NAME - 1] = '\0';
            _peers[i].ip       = ip;
            _peers[i].port     = port;
            _peers[i].lastSeen = millis();
            _peers[i].active   = true;
            return &_peers[i];
        }
    }
    return NULL;
}

uint16_t TrxNet::_nextMsgId() {
    uint16_t id = _msgId++;
    if (_msgId == 0) _msgId = 1;
    return id;
}
