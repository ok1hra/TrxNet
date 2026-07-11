#pragma once
#include <Arduino.h>
#include <Udp.h>

// ---------- library version ----------
// Bump on any API change. Apps may compile-check with #if TRXNET_VERSION >= ...
#define TRXNET_VERSION 0x0106   // 1.06 — isPriorityPeer() + parsePriorityPrefixes() helper (additive, wire-compatible)

// ---------- tuneable limits ----------
// All values can be overridden by defining them before including this header.
//
// NOTE on overrides: this #ifndef mechanism works only for header-only includes
// and PlatformIO `build_flags`. The Arduino IDE compiles library .cpp files in a
// separate translation unit that does NOT see #defines from the user's .ino —
// overriding from a sketch will silently break with C++ ODR violations (class
// size mismatch between sketch and library). To change a value with the Arduino
// IDE, edit this header directly (or the per-board blocks below).
//
// The three RAM-scaling limits (PEERS / PENDING / SEEN) default per board by SRAM
// class, so a strong MCU (ESP32, ~320 KB) tracks the whole network while a weak
// one (ATmega2560 8 KB, ATmega328 2 KB) keeps only what fits. On a full table,
// high-value devices can be protected with TrxNet::setPriorityPrefixes(). Override
// any single value to take full control.

// ============================================================================
// ABI WARNING — TRXNET_MAX_PEERS and TRXNET_MAX_PENDING size members of class
// TrxNet, so they change sizeof(TrxNet). Override them ONLY via a global build
// flag (e.g. -DTRXNET_MAX_PENDING=6 in platform/compiler options), which applies
// to every translation unit. NEVER `#define` them in a .ino/.cpp before including
// this header: TrxNet.cpp is compiled separately with the defaults, so the class
// layout would differ between the sketch and the library — the library ctor then
// initializes more array slots than the sketch allocated, corrupting ~kB of
// adjacent global memory (a classic ODR violation). The symptom is bizarre and
// far away, e.g. a LoadProhibited crash in the first nvs_open()/EEPROM.begin().
// begin() has a compile-/run-time guard that catches this — see below.
// ============================================================================

// Max peers held in the discovery table — the "how much of the network do I see"
// knob. A peer that does not fit is dropped (or evicts the stalest non-priority
// peer — see setPriorityPrefixes()). Each slot ~43 B on AVR.
#ifndef TRXNET_MAX_PEERS
  #if   defined(ESP32) || defined(ESP8266)
    #define TRXNET_MAX_PEERS   24
  #elif defined(__AVR_ATmega2560__)
    #define TRXNET_MAX_PEERS    8
  #else
    #define TRXNET_MAX_PEERS    4
  #endif
#endif
#ifndef TRXNET_MAX_SUBS
#define TRXNET_MAX_SUBS          8
#endif
#ifndef TRXNET_MAX_DEVICE_NAME
#define TRXNET_MAX_DEVICE_NAME   32   // including null terminator
#endif
#ifndef TRXNET_MAX_TOPIC_LEN
#define TRXNET_MAX_TOPIC_LEN     32   // including null terminator
#endif
// Canonical clamp for a single priority-prefix token (see setPriorityPrefixes /
// parsePriorityPrefixes). Device names are "TYPE.NN" with TYPE 2–3 chars, so a
// 4-char prefix uniquely selects a type. This does NOT size class TrxNet — the
// prefix buffer is caller-owned — so it is safe to override from a sketch.
#ifndef TRXNET_MAX_PRIO_PREFIX_LEN
#define TRXNET_MAX_PRIO_PREFIX_LEN  4
#endif
#ifndef TRXNET_MAX_PAYLOAD
#define TRXNET_MAX_PAYLOAD       64
#endif
// Shared pending queue for all outgoing CON messages.
// One publish(..., TRX_CON) to N peers consumes N slots; a per-peer greeting
// snapshot of T topics sent one peer per loop() consumes T slots plus retry
// headroom. Each slot costs ~130 B on AVR (TRXNET_PKT_MAX + metadata), making this
// the most RAM-hungry limit — size it to the app's worst-case CON burst, not
// blindly to peer count (e.g. the OI3 keyer greets 2 topics/peer → 8 is ample;
// the WX node greets 7 topics on ESP32 where 24 is trivially covered).
// NB: sizes class TrxNet — set via build flag only, never #define in a sketch.
// See the ABI WARNING above TRXNET_MAX_PEERS.
#ifndef TRXNET_MAX_PENDING
  #if   defined(ESP32) || defined(ESP8266)
    #define TRXNET_MAX_PENDING 24
  #elif defined(__AVR_ATmega2560__)
    #define TRXNET_MAX_PENDING  8
  #else
    #define TRXNET_MAX_PENDING  4
  #endif
#endif
// Dedup ring buffer for incoming CON messages.
// Must hold enough entries to cover the full retransmit window:
// TRXNET_CON_MAX_RETRIES retransmits × number of simultaneous senders.
#ifndef TRXNET_MAX_SEEN
  #if   defined(ESP32) || defined(ESP8266)
    #define TRXNET_MAX_SEEN    48
  #elif defined(__AVR_ATmega2560__)
    #define TRXNET_MAX_SEEN    16
  #else
    #define TRXNET_MAX_SEEN     8
  #endif
#endif

// ---------- timing (ms) ----------
#ifndef TRXNET_ANNOUNCE_MS
#define TRXNET_ANNOUNCE_MS       30000UL
#endif
#ifndef TRXNET_PEER_TIMEOUT_MS
#define TRXNET_PEER_TIMEOUT_MS   95000UL   // ~3 missed announces → peer removed
#endif
#ifndef TRXNET_CON_TIMEOUT_MS
#define TRXNET_CON_TIMEOUT_MS    2000UL
#endif
#ifndef TRXNET_CON_MAX_RETRIES
#define TRXNET_CON_MAX_RETRIES   3
#endif

// ---------- packet buffer ----------
// CoAP header(4) + options(TOPIC+8 overhead) + payload marker(1) + payload
#define TRXNET_PKT_MAX  (4 + TRXNET_MAX_TOPIC_LEN + 8 + 1 + TRXNET_MAX_PAYLOAD)

// ---------- public types ----------
enum TrxMsgType { TRX_NON = 0, TRX_CON = 1 };

struct TrxPeer {
    char      name[TRXNET_MAX_DEVICE_NAME];
    IPAddress ip;
    uint16_t  port;
    uint32_t  lastSeen;
    bool      active;
};

typedef void (*TrxNetCallback)(const char* from, const uint8_t* data, size_t len);

// Fired once when a new peer is discovered (first announce or first probe reply).
// NOT fired for known peers that simply refresh their lastSeen via a repeat announce.
// Fired again if the same peer is removed by timeout and later rejoins.
// ALSO fired (since 1.03) when a PROBE arrives from a peer that is already known:
// a PROBE is sent only from begin(), so this means the peer rebooted while still
// in our table (faster than the peer timeout). Use it to re-send a greeting
// snapshot so the rebooted peer gets fresh state without waiting. Make the
// handler idempotent — it may run for both brand-new and rebooted peers.
// Called from net.loop() during UDP packet handling — keep the body short and
// avoid calling publish/publishTo directly from here. Defer the work to the main
// loop (e.g. set a flag, queue the peer name) to stay clear of UDP re-entrancy.
typedef void (*TrxPeerCallback)(const TrxPeer* peer);

// ---------- ABI guard ----------
// abi_tag<N> is only *declared* here and *explicitly instantiated* in TrxNet.cpp
// for N == the library's own sizeof(TrxNet). begin()'s default argument ODR-uses
// abi_tag<sizeof(TrxNet)> as seen by the *caller's* translation unit, so if the
// sketch's sizeof(TrxNet) differs (a TRXNET_MAX_* mismatch, see ABI WARNING above)
// the link fails with "undefined reference to trxnet_detail::abi_tag<...>()".
// The same default arg also passes the caller's sizeof for a friendly run-time
// check in begin(). This turns silent memory corruption into a build/boot error.
namespace trxnet_detail { template <size_t N> size_t abi_tag(); }

// ---------- class ----------
class TrxNet {
public:
    // udp  : WiFiUDP or EthernetUDP instance (caller manages WiFi/Ethernet init)
    // port : UDP port for both discovery and CoAP. All devices must use the same port. Default: 5683.
    TrxNet(UDP& udp, uint16_t port = 5683);

    // Call once after network is up.
    // name : device identity string assembled at runtime, e.g. "transceiver.01". Max 31 chars.
    // callerAbi : leave defaulted — it carries the caller's sizeof(TrxNet) for the
    //             ABI guard (see trxnet_detail::abi_tag above). Do not pass it.
    void begin(const char* name,
               size_t callerAbi = (trxnet_detail::abi_tag<sizeof(TrxNet)>(), sizeof(TrxNet)));

    // Call from loop() — processes incoming packets, keepalive, CON retransmit
    void loop();

    // Register a callback for an incoming topic path, e.g. "/freq"
    // Registering the same path twice replaces the callback.
    void subscribe(const char* path, TrxNetCallback cb);

    // Remove subscription
    void unsubscribe(const char* path);

    // Send payload to all known peers on the given path.
    // TRX_NON: fire-and-forget. TRX_CON: retransmits until ACKed (per peer).
    void publish(const char* path, const uint8_t* data, size_t len,
                 TrxMsgType type = TRX_NON);

    // Send payload to one known peer by device name.
    // Returns false when peerName/path is invalid, the peer is not known, or the
    // CON pending queue is full.
    bool publishTo(const char* peerName, const char* path,
                   const uint8_t* data, size_t len,
                   TrxMsgType type = TRX_NON);

    // Number of currently active peers
    int peerCount() const;

    // Read-only access to peer by index (0..peerCount()-1). Returns NULL if out of range.
    const TrxPeer* peer(int index) const;

    // Set UDP port — call before begin(). Allows runtime port configuration from EEPROM.
    void setPort(uint16_t port);

    // Register a callback fired once per newly discovered peer. See TrxPeerCallback
    // notes above. Passing NULL clears the callback. Only one slot — registering
    // again replaces the previous callback.
    void onPeerAdded(TrxPeerCallback cb);

    // Protect high-value devices when the peer table (TRXNET_MAX_PEERS) is full.
    // `prefixes` is a caller-owned array of `count` name-prefix strings, matched
    // with strncmp — so "IC-705" matches "IC-705.01". When a peer whose name
    // matches a prefix announces and the table is full, the stalest NON-matching
    // peer is evicted to make room, so a RAM-constrained node keeps the devices
    // that matter and sheds the rest. Peers already in the table are never evicted
    // for a non-priority newcomer. The array must stay valid for the object's
    // lifetime — pass static string literals. Passing NULL/0 disables the feature.
    // No effect until the table actually fills. Note that dropping a peer only
    // stops THIS node from sending to it (publish/publishTo); incoming messages are
    // still received and dispatched to subscriptions regardless.
    void setPriorityPrefixes(const char* const* prefixes, uint8_t count);

    // Returns true if `name` matches one of the prefixes registered with
    // setPriorityPrefixes() (same strncmp prefix-match as the internal eviction
    // logic). Use it to MARK protected devices in a diagnostics/peer list — e.g.
    // for (int i=0;i<net.peerCount();i++) { const TrxPeer* p=net.peer(i);
    //     bool prot = net.isPriorityPeer(p->name); ... }
    // Reflects membership of the priority set, independent of whether the table
    // is currently full. Returns false when no prefixes are set. This is a pure
    // query — it adds no data member, so it does not change sizeof(TrxNet) and is
    // safe to call from any build (see INTEGRATION.md §10 "Compatibility").
    bool isPriorityPeer(const char* name) const;

    // Parse a space-separated priority-prefix string (e.g. "OI3 ANT ROT") into a
    // caller-owned, stable buffer + pointer array ready for setPriorityPrefixes().
    // Normalises exactly as INTEGRATION.md §5 prescribes: skips empty tokens,
    // upper-cases, and clamps each token to TRXNET_MAX_PRIO_PREFIX_LEN chars and
    // the list to `maxTokens`. Returns the token count written.
    //
    //   char        buf[8][TRXNET_MAX_PRIO_PREFIX_LEN + 1];   // stable storage
    //   const char* ptr[8];
    //   uint8_t n = TrxNet::parsePriorityPrefixes("oi3 ant", buf, ptr, 8);
    //   net.setPriorityPrefixes(n ? ptr : nullptr, n);        // before begin()
    //
    // `buf` and `ptr` MUST outlive the TrxNet object (the library keeps `ptr`).
    // Static (no `this`) and touches only caller memory, so it is ABI-neutral.
    static uint8_t parsePriorityPrefixes(
        const char* src,
        char        (*buf)[TRXNET_MAX_PRIO_PREFIX_LEN + 1],
        const char* *ptr,
        uint8_t     maxTokens);

private:
    struct Sub {
        char           path[TRXNET_MAX_TOPIC_LEN];
        TrxNetCallback cb;
        bool           active;
    };

    struct Pending {
        uint8_t   buf[TRXNET_PKT_MAX];
        size_t    len;
        IPAddress ip;
        uint16_t  port;
        uint16_t  msgId;
        uint32_t  sentAt;
        uint8_t   retries;
        bool      active;
    };

    // Ring buffer for CON deduplication — tracks recently received (ip, msgId) pairs.
    // Prevents duplicate callback dispatch when a retransmitted CON arrives after
    // the ACK was lost in transit.
    struct SeenMsg {
        IPAddress ip;
        uint16_t  msgId;
    };

    UDP&      _udp;
    char      _name[TRXNET_MAX_DEVICE_NAME];
    uint16_t  _port;
    uint32_t  _lastAnnounce;
    uint16_t  _msgId;

    TrxPeer  _peers[TRXNET_MAX_PEERS];
    Sub      _subs[TRXNET_MAX_SUBS];
    Pending  _pending[TRXNET_MAX_PENDING];
    SeenMsg  _seen[TRXNET_MAX_SEEN];
    uint8_t  _seenIdx;

    TrxPeerCallback _onPeerAdded;

    const char* const* _prio;        // caller-owned array of priority name prefixes
    uint8_t            _prioCount;

    bool      _isPriority(const char* name) const;
    void      _sendDiscovery(uint8_t pktType, IPAddress dest);
    void      _handleIncoming();
    void      _processDiscovery(IPAddress src, const uint8_t* buf, size_t len);
    void      _processCoAP(IPAddress src, uint16_t srcPort,
                            const uint8_t* buf, size_t len);
    size_t    _buildCoAP(uint8_t* buf, const char* path,
                          const uint8_t* data, size_t dataLen,
                          TrxMsgType type, uint16_t msgId);
    void      _sendRaw(IPAddress ip, uint16_t port,
                        const uint8_t* buf, size_t len);
    void      _sendACK(IPAddress ip, uint16_t port, uint16_t msgId);
    void      _checkTimeouts();
    TrxPeer*  _findOrAddPeer(const char* name, IPAddress ip, uint16_t port);
    uint16_t  _nextMsgId();
};
