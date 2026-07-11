# TrxNet Integration Profile

**A normative, form-independent profile for TrxNet devices.**

This document defines *how* a device should expose TrxNet — its settings, its
publish/subscribe roles, and its diagnostics — so that every device in the
remoteQTH family behaves consistently regardless of its user interface (web page,
LCD menu, serial console, headless). The wire protocol and the library API are
specified in [README.md](README.md); this document sits one layer above and
prescribes the *application-level contract*.

It is derived from a survey of seven shipping implementations
(705, OI3, ROT, DIN, WX, INK, ANT) and is intended to be the convergence target
they are refactored toward.

---

## 1. Scope and conformance keywords

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are
used as in RFC 2119.

- **MUST** requirements are needed for *interoperability* — a device that
  violates one can silently break the network for others (wrong enable
  semantics, wrong topic direction, wrong payload encoding).
- **SHOULD** requirements are for *consistency* of configuration and diagnostics
  across the fleet. A device with no display or no web UI MAY omit the *form* of
  a SHOULD, but MUST still honour the underlying data/behaviour where it applies.

"Form-independent" means: this profile prescribes **settings, functions and
data**, never **how** they are rendered. A setting defined here as
`publish_enable` may be a web checkbox, an LCD menu item, or a serial command —
the profile does not care, as long as the semantics match.

---

## 2. Device identity and naming

Every device has a **type** and a **NET_ID**, combined into a network name.

```
name = "<TYPE>.<NET_ID as 2-digit lowercase hex>"     e.g.  705.01, OI3.ff, ROT.0a
```

- **TYPE** is fixed by the firmware (a short 2–3 char string). It is **not** a
  user setting. It MUST be stable for a given product.
- **NET_ID** is a user setting, `uint8_t`, range `0x00`–`0xff`. `0x00` is the
  reserved **disabled sentinel** (see §3).
- The name MUST be assembled at runtime (from EEPROM/NVS), not hard-coded, and
  passed to `net.begin(name)`. Max length `TRXNET_MAX_DEVICE_NAME - 1` (31).

### Reserved type prefixes

| TYPE  | Device                              |
|-------|-------------------------------------|
| `705` | IC-705 Interface / QRPlog           |
| `OI3` | Open Interface III (k3ng CW keyer)  |
| `ROT` | IP-rotator                          |
| `DIN` | ETH DIN-rail dev kit (TrxNetSwitch) |
| `WX`  | Weather station                     |
| `INK` | e-ink telemetry display             |
| `ANT` | AntHub-NET antenna matrix           |

New types MUST NOT collide on their first 4 characters with an existing type
(priority prefixes match on ≤4 chars — see §5).

---

## 3. Enable model

A device decides whether to join the network from **two** settings that combine
with a hard interlock.

| Setting  | Type   | Meaning                                             |
|----------|--------|-----------------------------------------------------|
| `net_id` | uint8  | Device ID; `0x00` = disabled sentinel.              |
| `enable` | bool   | Master on/off switch for the TrxNet stack.          |

**Rules (MUST):**

1. The device joins the network — i.e. calls `net.begin()` — **iff**
   `enable == true` **AND** `net_id != 0x00`.
2. When `net_id == 0x00`, `enable` is **forced off and locked**: the UI MUST
   present it as un-togglable (greyed/read-only), and firmware MUST treat any
   attempt to set `enable = true` while `net_id == 0` as a no-op.
3. `enable` and `net_id` are **independent stored values**. Turning a device off
   via `enable` MUST preserve its `net_id`, so the previously configured ID
   returns when it is re-enabled. (This is why a bare sentinel is not enough.)

> **Rationale.** The sentinel alone (used today by 705/OI3) cannot express
> "disabled but remember my ID". A separate enable flag alone (WX byte 35) lets
> a user create the nonsensical state *enabled with ID 0*. Combining them, with
> the sentinel as a hard interlock over the flag, gives both a persistent ID and
> a coherent off state.

Devices for which TrxNet is only **one of several transports** (INK: MQTT vs
TrxNet; DIN: TrxNetSwitch is one module mode) MAY gate the whole stack behind a
higher-level mode selector, but once TrxNet is the selected transport they MUST
still honour §3.1–§3.3 within that branch.

---

## 4. Canonical settings

Every TrxNet device MUST expose the following logical settings. Only the
**names, types, ranges, defaults and semantics** are normative — the **physical
storage** (EEPROM offset, NVS key) is left to the device, and legacy layouts are
not required to move. New ESP32 devices SHOULD use these exact NVS key names.

| Key                 | Type          | Range / format                | Default            | Notes |
|---------------------|---------------|-------------------------------|--------------------|-------|
| `net_id`            | uint8         | `0x00`–`0xff`                 | product-specific   | `0x00` = disabled (§3). |
| `enable`            | bool          | on/off                        | `off`              | Locked off when `net_id==0` (§3). |
| `port`              | uint16        | `1`–`65534`                   | `5683`             | Same on all peers; different port = separate network. |
| `publish_enable`    | bool          | on/off                        | role default (§6)  | Gates the outbound (state) direction. |
| `subscribe_enable`  | bool          | on/off                        | role default (§6)  | Gates the inbound (command) direction. |
| `priority_prefixes` | string        | space-separated, ≤8×4 chars   | product-specific   | See §5. Empty = feature off. |

- A device MUST NOT invent a differently-named setting for the same concept
  (e.g. `proto`, `Configuration==4`, enable-byte-35 all collapse into
  `net_id`+`enable` per §3).
- `publish_enable` / `subscribe_enable` MUST only be shown for directions the
  firmware actually supports (§6).
- Devices MAY add product-specific settings beyond this core (e.g. **target
  bindings**, §9) — but the six keys above are the shared vocabulary.

---

## 5. Priority prefixes

`priority_prefixes` protects high-value peers from eviction when the peer table
(`TRXNET_MAX_PEERS`) fills — critical on the AVR node (OI3, table = 8), harmless
on ESP32 (table = 24). It maps directly onto
`TrxNet::setPriorityPrefixes()`.

**Canonical format (MUST):**

- A single **space-separated** string, e.g. `"OI3 ANT ROT"`.
- Up to **8 prefixes**, each up to **4 characters** (`TRXNET_MAX_PRIO_PREFIX_LEN`).
- Normalised: trim, collapse internal whitespace, upper-case, clamp each token to
  4 chars and the list to 8 tokens. Stored form == applied form.
- Matched with `strncmp` (prefix match), so `"705"` matches `705.01`, `705.ff`, …
- Tokenised into a **stable buffer** whose lifetime ≥ the `TrxNet` object
  (spaces replaced by `\0`, pointer array handed to the library). The library
  keeps the pointers — they MUST NOT dangle.
- An empty string means **feature off** (`setPriorityPrefixes(NULL, 0)`).

`setPriorityPrefixes()` MUST be called **before** `begin()`.

> **4 chars is enough.** Real names are `TYPE.NN` where TYPE is 2–3 chars, so a
> 4-char prefix uniquely selects a type (and the README's 6-char `"IC-705"`
> example is longer than any real name `705.01`).

### Recommended library extension (see §10)

Today every device re-implements this normalisation with different limits
(8×8, 3×8, 8×5, 32-char). The profile RECOMMENDS moving it into the library:

- define `TRXNET_MAX_PRIO_PREFIX_LEN` (= 4) as the canonical clamp, and
- provide a helper that parses a space-separated string into the stable buffer
  + pointer array, so all devices share identical behaviour.

Until that lands, each device MUST implement the §5 rules identically.

---

## 6. Publish / subscribe split

TrxNet traffic is split into two independent directions, each with its own
runtime switch.

### 6.1 Topic direction convention (MUST)

| Topic form | Direction | Meaning                    | Examples |
|------------|-----------|----------------------------|----------|
| `/x`       | **state** | published by the *owner* of the value | `/hz`, `/mode`, `/gpio`, `/azimuth`, `/temp` |
| `/s-x`     | **command** (set-point) | subscribed by the *target* device | `/s-hz`, `/s-mode`, `/s-gpio`, `/s-azimuth`, `/s-cw` |

`s-` reads as "set". A device publishing `/hz` announces *its* frequency; a
device subscribing `/s-hz` accepts a *commanded* frequency. This makes the two
directions machine-separable and lets the two switches below be defined cleanly.

### 6.2 The two switches (MUST)

- **`publish_enable`** gates the **outbound state** direction: periodic
  `publish("/x", …)`, change-driven publishes, **and** the new-peer greeting
  snapshot (`onPeerAdded` → `publishTo`). When off, the device announces its
  presence (discovery keepalive still runs) but sends **no** state.
- **`subscribe_enable`** gates the **inbound command** direction: the device
  registers its `/s-x` subscriptions only when on. When off, it does not act on
  commands.

Notes:

- Discovery/keepalive and diagnostics (§8) run whenever the stack is enabled
  (§3), independent of both switches.
- A device MUST expose only the switches for directions it implements:
  a publish-only node (WX) hides `subscribe_enable`; a subscribe-only node (INK)
  hides `publish_enable`.
- The greeting snapshot is part of the **publish** direction — it MUST NOT be
  sent when `publish_enable` is off.

### 6.3 Recommended code structure (SHOULD)

Mirror the split in code with two functions:

```cpp
void publishState();   // one place that emits every /x this device owns
void subscribeAll();   // one place that registers every /s-x this device accepts
```

`publishState()` is called both periodically/on-change **and** from the
per-peer greet drain, so a newly joined peer gets the full state snapshot with
no duplicated topic list. `subscribeAll()` is called once from setup after
`begin()`.

---

## 7. Topic contract and payload encodings

All payloads are **raw little-endian bytes**; the library never interprets them.
Every callback MUST guard length (`if (len < sizeof(T)) return;`). The
authoritative encoding tables live in [README.md](README.md#conventions); the
canonical set is summarised here with its direction:

| Topic       | Dir | Type      | Encoding                              |
|-------------|-----|-----------|---------------------------------------|
| `/hz`       | pub | uint32    | frequency in Hz                       |
| `/mode`     | pub | uint8     | ICOM CI-V mode byte                   |
| `/flags`    | pub | uint16    | bitfield                              |
| `/azimuth`  | pub | uint16    | degrees                               |
| `/elevation`| pub | uint16    | degrees                               |
| `/gpio`     | pub | uint8     | 8-bit output map (state echo)         |
| `/temp`     | pub | int16     | °C × 100                              |
| `/hum` `/press` `/rain` `/winddir` `/windavg` `/windmax` | pub | uint16 | scaled ints (see README WX table) |
| `/s-hz`     | sub | uint32    | commanded frequency                   |
| `/s-mode`   | sub | uint8     | commanded CI-V mode                   |
| `/s-azimuth`/`/s-elevation` | sub | uint16 | commanded degrees          |
| `/s-gpio`   | sub | uint8     | commanded 8-bit output map            |
| `/s-cw`     | sub | char[]    | CW/text, `TRX_CON`; single `0x00` byte = "stop sending" |

A new topic MUST follow §6.1 direction naming and MUST document its encoding
here before use across devices.

`TRX_CON` (retransmit-until-ACK) MUST be used for text/command payloads that must
not be lost (`/s-cw`, greeting snapshots). `TRX_NON` (fire-and-forget) SHOULD be
used for periodic telemetry where the next update supersedes a lost one.

---

## 8. Diagnostics

Every enabled device MUST make the following diagnostic model available in some
form (web JSON, LCD browse, serial dump — form is free). The **data** is
normative; the **rendering** is not.

### 8.1 Data model

**Per peer** (for each entry `0 … peerCount()-1`):

| Field      | Source                                   | Meaning |
|------------|------------------------------------------|---------|
| `name`     | `peer->name`                             | device name `TYPE.NN` |
| `ip`       | `peer->ip`                               | IPv4 |
| `port`     | `peer->port`                             | UDP port |
| `ageMs`    | `millis() - peer->lastSeen`              | ms since last announce |
| `priority` | `net.isPriorityPeer(name)` (§10)         | true = protected from eviction |
| `self`     | `name == own name`                       | true for this device's own entry (if listed) |

**Global:**

| Field        | Source                          | Meaning |
|--------------|---------------------------------|---------|
| `name`       | own device name                 | identity |
| `port`       | `port` setting                  | active port |
| `enabled`    | §3 state                        | stack running |
| `publishOn`  | `publish_enable`                | outbound state on |
| `subscribeOn`| `subscribe_enable`              | inbound command on |
| `peerCount`  | `net.peerCount()`               | active peers |
| `peerMax`    | `TRXNET_MAX_PEERS`              | table capacity |
| `tableFull`  | `peerCount >= peerMax`         | warning: peers are being dropped |

`tableFull` is the key troubleshooting signal — it tells the operator *why* a
device X is not visible (the table filled and X was evicted or never admitted).
Prioritised peers MUST be visibly marked in whatever list form is used.

### 8.2 Serialisation (SHOULD)

Devices with a web/API surface SHOULD serialise the model with these canonical
JSON keys, so the TrxNet Monitor, NodeRed and companion UIs read one shape:

```json
{
  "self": { "name": "705.01", "port": 5683, "enabled": true,
            "publishOn": true, "subscribeOn": true,
            "peerCount": 3, "peerMax": 24, "tableFull": false },
  "peers": [
    { "name": "OI3.ff", "ip": "10.0.0.7", "port": 5683,
      "ageMs": 4200, "priority": true,  "self": false },
    { "name": "ANT.01", "ip": "10.0.0.9", "port": 5683,
      "ageMs": 1100, "priority": true,  "self": false }
  ]
}
```

LCD/serial forms are free (e.g. OI3's "Peers" menu browsing IP tails). They MUST
still surface `name`, reachability (age), and the `priority` mark.

---

## 9. Target bindings (per-device pattern, non-core)

Some devices send **directed** commands to a *named* peer via `publishTo()`,
distinct from broadcast `publish()`:

- 705 → OI3 keyer(s): `publishTo(OI3.<id>, "/s-cw"|"/s-hz", …)` (settings
  `TRX2_NET_ID`, `TRX3_NET_ID`)
- ANT → DIN: `publishTo(DIN.<id>, "/s-gpio", …)` (setting `trxnetDinName`)

This is a real need but is **not** part of the six-key core (§4). A device that
needs it SHOULD model it as **0..N named target slots**, each `{label, target
NET_ID or name}`, default empty/0, and SHOULD list its own targets as separate
priority prefixes so they survive table pressure. It MUST use the §6.1 `/s-x`
naming for the directed topics.

---

## 10. Library extensions (shipped in v1.06)

To let devices implement §5 and §8 without duplicating logic, the library gained
two **additive** members in **v1.06** (`TRXNET_VERSION >= 0x0106`):

1. **`bool TrxNet::isPriorityPeer(const char* name) const`** — public, mirrors
   the internal `_isPriority()` prefix match. Enables the `priority` diagnostic
   field (§8) without the app re-implementing matching. It reflects *membership
   of the priority set*, regardless of whether the table is currently full.
2. **`TRXNET_MAX_PRIO_PREFIX_LEN` (= 4)** plus the static parser
   **`TrxNet::parsePriorityPrefixes(src, buf, ptr, maxTokens)`**, which
   normalises a space-separated string exactly as §5 prescribes (skip empty
   tokens, upper-case, clamp each token to 4 chars and the list to `maxTokens`)
   into a caller-owned stable buffer + pointer array ready for
   `setPriorityPrefixes()`. All devices thus share identical behaviour.

```cpp
// One-time setup, replaces each device's hand-rolled normaliser:
static char        prioBuf[8][TRXNET_MAX_PRIO_PREFIX_LEN + 1];  // stable storage
static const char* prioPtr[8];
uint8_t n = TrxNet::parsePriorityPrefixes(prioStr, prioBuf, prioPtr, 8);
net.setPriorityPrefixes(n ? prioPtr : nullptr, n);              // before begin()
```

> **`bool priority` in `TrxPeer` was rejected** as an alternative: adding a data
> member changes `sizeof(TrxPeer)`/`sizeof(TrxNet)` and trips the ABI guard — see
> Compatibility below. The pure method carries the same information at zero
> layout cost.

### 10.1 Compatibility

Three independent kinds of compatibility, deliberately all preserved:

**A. Between devices on the network (mixing library versions).**
The discovery "hello" packet carries a **protocol-version byte** (`DISC_VERSION`,
currently `0x01`) and the receiver drops any packet whose byte differs — so two
devices interoperate **only if `DISC_VERSION` matches**. The v1.06 extensions add
**nothing to any packet** and priority is a purely *local, receiver-side* decision
(who *I* protect in *my* table, who *I* reach — it never travels on the wire).
Therefore:

> ✅ **Old and new devices see each other and exchange topics identically. Mixed-
> version networks work.** New features appear only on devices you actually
> reflash; the rest are unaffected.

**MUST NOT** bump `DISC_VERSION` for a change that leaves the packet layout
unchanged — doing so would silently partition the network into version islands.

**B. Inside one binary (sketch translation unit vs library `.cpp`).**
The `TRXNET_MAX_*` macros that size class `TrxNet` carry the ODR hazard the header
warns about, guarded since v1.05. The v1.06 additions are chosen to be
**layout-neutral**:

| Addition | Sizes the class? | Effect |
|----------|:----------------:|--------|
| `isPriorityPeer()` (method) | no | ABI-neutral; old sketches link unchanged |
| `parsePriorityPrefixes()` (static) | no | touches only caller memory |
| `TRXNET_MAX_PRIO_PREFIX_LEN` | **no** | the prefix buffer is caller-owned; the macro must never size a `TrxNet` member |

**C. Source (recompiling an old sketch against the new header).**
Purely additive API + a non-sizing macro → old sketches compile unchanged.
`TRXNET_VERSION` is bumped to `0x0106` so sketches can gate new calls with
`#if TRXNET_VERSION >= 0x0106` (as OI3 already does for `0x0104`).

**Forward-compatibility note.** `_processDiscovery()` today rejects a mismatched
`DISC_VERSION` with a hard `!=`, so the protocol is *not* forward-compatible: any
future version bump splits the network. If the wire format ever must change,
accept a **compatible range** of versions and make new fields **trailing/optional**
so old parsers ignore rather than drop them. Out of scope for these extensions
(they leave the wire untouched), but noted here as the migration rule.

---

## 11. Per-device profiles

Platform note: **OI3 is the only AVR node** (ATmega2560, `TRXNET_MAX_PEERS = 8`,
so priority prefixes matter most there). All others are ESP32 (`= 24`).

| Device | Platform | Role | Publishes | Subscribes | `publish_enable` default | `subscribe_enable` default | Priority default | Target bindings |
|--------|----------|------|-----------|------------|--------------------------|----------------------------|------------------|-----------------|
| **705** | ESP32 | pub + sub + commander | `/hz` | `/hz`, `/mode`, `/s-hz` | on | on | `OI3 ANT` | `TRX2/3_NET_ID` → OI3 `/s-cw`,`/s-hz` |
| **OI3** | ATmega2560 | pub + sub | `/hz`, `/mode` | `/s-hz`, `/s-mode`, `/s-cw` | on | on | `705 ANT ROT` | — |
| **ROT** | ESP32 | pub + optional sub | `/azimuth`, `/elevation` | `/s-azimuth`, `/s-elevation` | on | **off** (legacy `TrxNetSubEnabled`) | `INK 705` | — |
| **DIN** | ESP32 | sub + echo | `/gpio` (echo) | `/s-gpio` | on (echo) | on | `ANT` | — |
| **WX**  | ESP32 | publisher-only | 7 WX topics | — | on | *(hidden)* | `INK` | — |
| **INK** | ESP32 | subscriber-only | — | up to 8 arbitrary `/x` paths (dynamic) | *(hidden)* | on | type of mirrored source (`WX`/`ROT`/…) | — |
| **ANT** | ESP32 | sub + commander | — | `/hz`, `/gpio` | *(hidden)* | on | `OI3 705` | `trxnetDinName` → DIN `/s-gpio` |

Notes:
- **INK** is the generalised subscriber: it maps N configured topic paths to
  trampoline callbacks and only displays. It has no publish direction, so
  `publish_enable` is hidden and its greeting behaviour is N/A.
- **WX** is the pure publisher: it greets new peers with a `TRX_CON` snapshot of
  all 7 topics, one peer per loop iteration.
- **ROT**'s existing `TrxNetSubEnabled` **is** `subscribe_enable`; it should be
  renamed to converge, keeping default off (a rotator ignores remote targets
  unless the operator opts in).

---

## 12. Conformance matrix

Snapshot of where the fleet stands against this profile.
✓ = conforms, ✗ = diverges (needs migration), ~ = partial, N/A = not applicable.

| Requirement | 705 | OI3 | ROT | DIN | WX | INK | ANT |
|-------------|:---:|:---:|:---:|:---:|:--:|:---:|:---:|
| §3 `net_id`+`enable` interlock | ~¹ | ~¹ | ~¹ | ✗² | ✗³ | ✗⁴ | ~¹ |
| §4 canonical setting names | ~ | ~ | ~ | ✗ | ✗ | ✗ | ~ |
| §5 priority format 8×4 + normalise | ✗⁵ | ~⁶ | ✗⁵ | ~⁷ | ✗⁸ | ✗ | ✗ |
| §6.1 topic direction `/x` `/s-x` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| §6.2 `publish_enable` switch | ✗ | ✗ | ✗ | ✗ | ✗ | N/A | N/A |
| §6.2 `subscribe_enable` switch | ✗ | ✗ | ✓ | ✗ | N/A | ✗ | ✗ |
| §8 diagnostics: peer list | ✓ | ~⁹ | ✓ | ✗ | ✗ | ✓ | ✗ |
| §8 diagnostics: priority mark | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| §8.2 canonical JSON keys | ✗ | N/A | ✗ | N/A | ✗ | ✗ | N/A |
| §10 uses `isPriorityPeer()` | ✗ | ~⁶ | ✗ | ✗ | ✗ | ✗ | ✗ |

¹ Sentinel only — no separate persistent `enable`; add the interlocked flag.
² Uses `Configuration==4` module mode instead of `net_id`+`enable`.
³ Uses a separate enable byte (35) *and* NET_ID, but not interlocked per §3.
⁴ Uses `proto` (MQTT vs TrxNet) selector; wrap TrxNet branch in §3 semantics.
⁵ 8×8 today — clamp to 4 chars.
⁶ OI3 already has a local `_isPriority`-equivalent and 3-slot prefix list; align length + adopt library helper.
⁷ 3×8 today — widen to 8, clamp to 4.
⁸ 8×5 today — clamp to 4.
⁹ LCD browse of peer IP tails; no priority mark, no capacity readout.

**Priority migrations (highest value first):**
1. Add the §8 **priority mark** to every peer list (needs §10.1 lib extension) —
   this is the headline feature requested and currently ✗ everywhere.
2. Land the §10 library extensions (`isPriorityPeer`, prefix parser + 4-char
   clamp) so §5/§8 stop being re-implemented per device.
3. Converge the enable model (§3) — DIN, WX, INK are the biggest deviations.
4. Add the missing `publish_enable` switch fleet-wide and rename ROT's
   `TrxNetSubEnabled` → `subscribe_enable`.
