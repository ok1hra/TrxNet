Hlavní provozní rizika jsou už v README zmíněná, ale dvě oblasti bych ještě zpřesnil, aby uživatel nepřeceňoval spolehlivost a lépe chápal limity nasazení.

Doplnil bych hlavně toto:

1. U `TRX_CON` bych explicitně napsal, že jde prakticky o “best effort reliability” nad UDP, ne tvrdou garanci doručení.
Navržená formulace:
```md
- `TRX_CON` improves reliability by retransmitting until ACK, but it is not a hard delivery guarantee.
- Delivery can still fail if the peer is offline, the pending queue is full, or the receiver loop is blocked too long.
- Duplicate callbacks are prevented only within the capacity of `TRXNET_MAX_SEEN`.
```

2. U pending queue bych zdůraznil dopad na broadcast do více peerů.
Navržená formulace:
```md
- A single `publish(..., TRX_CON)` to many peers can consume the entire pending queue immediately.
- If the queue is full, additional CON sends are silently dropped.
- For multi-peer deployments, size `TRXNET_MAX_PENDING` for peak fan-out, not average traffic.
```

3. U discovery bych doplnil, že problém nemusí být jen router/subnet, ale i samotná WiFi infrastruktura.
Navržená formulace:
```md
- Discovery depends on local broadcast delivery.
- Some WiFi networks, guest networks, mesh systems, VLANs, or AP isolation settings can break peer discovery even when normal IP connectivity works.
- Always validate discovery on the exact target network before deployment.
```

4. Chybí praktické doporučení pro nasazení na AVR.
Navržená formulace:
```md
- On ATMEGA2560, review RAM usage before enabling larger payloads, pending queues, or dedup buffers.
- Increasing `TRXNET_MAX_PENDING` and `TRXNET_MAX_SEEN` improves robustness but directly increases static RAM usage.
```

5. Tiché zahazování bych popsal jako behavior API, ne jen detail.
Navržená formulace:
```md
- Invalid usage is rejected silently: too-long topic paths are ignored, oversized payloads are truncated, and excess CON traffic may be dropped when the queue is full.
- If your application needs diagnostics, add wrapper checks in application code before calling `publish()` or `subscribe()`.
```

Shrnutí: dokumentace je už solidní, ale ještě bych víc narovnal očekávání kolem `TRX_CON`, fan-outu do více peerů a ověření na cílové síti. Pokud chceš, připravím ti rovnou přesný patch README bez úprav kódu.
