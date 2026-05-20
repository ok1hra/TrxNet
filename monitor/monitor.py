#!/usr/bin/env python3
"""TrxNet Monitor — passive UDP sniffer + WebSocket dashboard."""

import argparse
import asyncio
import collections
import dataclasses
import json
import os
import random
import socket
import struct
import time
import webbrowser
from http import HTTPStatus
from pathlib import Path
from typing import Optional

try:
    import websockets
    import websockets.server
    from websockets.datastructures import Headers
    from websockets.http11 import Response as WsResponse
except ImportError:
    raise SystemExit("Missing dependency: pip install 'websockets>=12'")

# ── protocol constants ────────────────────────────────────────────────────────

DISC_MAGIC    = 0xAA
DISC_VERSION  = 0x01
DISC_PROBE    = 0x01
DISC_ANNOUNCE = 0x02

COAP_VER      = 1
COAP_CON      = 0
COAP_NON      = 1
COAP_ACK      = 2
COAP_POST     = 0x02
COAP_EMPTY    = 0x00
COAP_URI_PATH = 11

ANNOUNCE_INTERVAL       = 30.0    # s between JOIN NETWORK announces
PEER_TIMEOUT            = 95.0    # s — matches TRXNET_PEER_TIMEOUT_MS
PEER_TIMEOUT_WARN       = 60.0    # s — warn at half-timeout
CON_TIMEOUT             = 2.0     # s per INJECT CON retry
CON_MAX_RETRIES         = 3
PACKET_RING_SIZE        = 500
DEVICE_MSG_HISTORY      = 20
RATE_WINDOW             = 10.0    # s sliding window for packet rate
STATS_INTERVAL          = 1.0     # s between stats ticks
DISCOVERY_ALERT_TIMEOUT = 120.0   # s without any discovery → alert
CON_CHAIN_EXPIRE        = 10.0    # s after which a retransmit chain is forgotten

CI_V_MODES = {
    0x00: "LSB",  0x01: "USB",    0x02: "AM",    0x03: "CW",
    0x04: "RTTY", 0x05: "FM",    0x06: "WFM",   0x07: "CW-R",
    0x08: "RTTY-R", 0x11: "DV",  0x12: "DD",
}

CI_V_FLAGS = [
    (0,  "PTT"),  (1,  "SPLIT"), (2,  "RIT"),  (3,  "XIT"),
    (4,  "AFC"),  (5,  "NB"),    (6,  "NR"),   (7,  "COMP"),
    (8,  "MONI"), (9,  "VOX"),   (10, "SCAN"),
]

# ── payload decoder ───────────────────────────────────────────────────────────

def decode_payload(topic: str, data: bytes) -> str:
    if not data:
        return "(empty)"
    try:
        if topic in ("/freq", "/hz"):
            hz = struct.unpack_from("<I", data)[0]
            return f"{hz / 1_000_000:.3f} MHz"
        if topic == "/mode":
            return CI_V_MODES.get(data[0], f"0x{data[0]:02X}")
        if topic == "/flags":
            val = struct.unpack_from("<H", data)[0]
            flags = [name for bit, name in CI_V_FLAGS if val & (1 << bit)]
            return (" | ".join(flags)) if flags else "0x0000"
        if topic in ("/cw", "/s-cw"):
            return f'"{data.decode("ascii", errors="replace")}"'
        if topic in ("/s-hz",):
            hz = struct.unpack_from("<I", data)[0]
            return f"{hz / 1_000_000:.3f} MHz"
        if topic in ("/s-mode",):
            return CI_V_MODES.get(data[0], f"0x{data[0]:02X}")
    except Exception:
        pass
    return "0x " + " ".join(f"{b:02X}" for b in data)

# ── protocol parsers ──────────────────────────────────────────────────────────

def parse_discovery(data: bytes) -> Optional[dict]:
    if len(data) < 4 or data[0] != DISC_MAGIC or data[1] != DISC_VERSION:
        return None
    pkt_type = data[2]
    name_len = data[3]
    if len(data) < 4 + name_len + 2:
        return None
    name = data[4:4 + name_len].decode("utf-8", errors="replace")
    port = (data[4 + name_len] << 8) | data[4 + name_len + 1]
    return {
        "disc_type": "probe" if pkt_type == DISC_PROBE else "announce",
        "name": name,
        "port": port,
    }

def parse_coap(data: bytes) -> Optional[dict]:
    if len(data) < 4:
        return None
    if ((data[0] >> 6) & 0x03) != COAP_VER:
        return None
    typ   = (data[0] >> 4) & 0x03
    tkl   = data[0] & 0x0F
    code  = data[1]
    msg_id = (data[2] << 8) | data[3]

    if typ == COAP_ACK:
        return {"kind": "ack", "msg_id": msg_id}

    if code != COAP_POST:
        return None

    pos = 4 + tkl
    if pos > len(data):
        return None

    path_parts: list[str] = []
    opt_num = 0
    while pos < len(data) and data[pos] != 0xFF:
        d = (data[pos] >> 4) & 0x0F
        l = data[pos] & 0x0F
        pos += 1
        if d == 13:
            if pos >= len(data): return None
            d = data[pos] + 13;  pos += 1
        elif d == 14:
            if pos + 1 >= len(data): return None
            d = ((data[pos] << 8) | data[pos + 1]) + 269; pos += 2
        if l == 13:
            if pos >= len(data): return None
            l = data[pos] + 13;  pos += 1
        elif l == 14:
            if pos + 1 >= len(data): return None
            l = ((data[pos] << 8) | data[pos + 1]) + 269; pos += 2
        opt_num += d
        if opt_num == COAP_URI_PATH and l > 0:
            if pos + l > len(data): return None
            path_parts.append(data[pos:pos + l].decode("utf-8", errors="replace"))
        pos += l

    topic = "/" + "/".join(path_parts) if path_parts else "/"

    payload = b""
    if pos < len(data) and data[pos] == 0xFF:
        payload = data[pos + 1:]

    msg_type_str = {COAP_CON: "CON", COAP_NON: "NON"}.get(typ, "UNK")
    return {
        "kind": "post",
        "msg_type": msg_type_str,
        "msg_id": msg_id,
        "topic": topic,
        "payload": payload,
        "payload_truncated": len(payload) > 64,
    }

# ── CoAP / discovery builders ─────────────────────────────────────────────────

def build_discovery(pkt_type: int, name: str, port: int) -> bytes:
    enc = name.encode("utf-8")[:31]
    buf = bytes([DISC_MAGIC, DISC_VERSION, pkt_type, len(enc)]) + enc
    buf += bytes([port >> 8, port & 0xFF])
    return buf

def build_coap(topic: str, payload: bytes, msg_type: str, msg_id: int) -> bytes:
    typ = COAP_CON if msg_type == "CON" else COAP_NON
    buf = bytearray([
        (COAP_VER << 6) | (typ << 4),
        COAP_POST,
        msg_id >> 8, msg_id & 0xFF,
    ])
    parts = [p for p in topic.lstrip("/").split("/") if p]
    prev_opt = 0
    for part in parts:
        seg = part.encode("utf-8")
        delta = COAP_URI_PATH - prev_opt
        prev_opt = COAP_URI_PATH
        d_nib = delta if delta < 13 else 13
        l_nib = len(seg) if len(seg) < 13 else 13
        buf.append((d_nib << 4) | l_nib)
        if delta >= 13:  buf.append(delta - 13)
        if len(seg) >= 13: buf.append(len(seg) - 13)
        buf += seg
    if payload:
        buf.append(0xFF)
        buf += payload
    return bytes(buf)

def build_ack(msg_id: int) -> bytes:
    return bytes([(COAP_VER << 6) | (COAP_ACK << 4), COAP_EMPTY, msg_id >> 8, msg_id & 0xFF])

def encode_inject_value(value_type: str, value_str: str) -> bytes:
    try:
        if value_type == "uint32": return struct.pack("<I", int(value_str))
        if value_type == "uint16": return struct.pack("<H", int(value_str))
        if value_type == "uint8":  return struct.pack("B", int(value_str))
        if value_type == "string": return value_str.encode("ascii")
        if value_type == "raw":    return bytes.fromhex(value_str.replace("0x","").replace(" ",""))
    except Exception:
        pass
    return b""

# ── state model ───────────────────────────────────────────────────────────────

@dataclasses.dataclass
class DeviceMsg:
    ts: float
    topic: str
    decoded: str
    msg_type: str

@dataclasses.dataclass
class Device:
    name: str
    ip: str
    port: int
    first_seen: float
    last_seen: float       = 0.0
    last_announce: float   = 0.0
    packet_count: int      = 0
    publishes: dict        = dataclasses.field(default_factory=dict)
    receives: dict         = dataclasses.field(default_factory=dict)
    messages: object       = dataclasses.field(
        default_factory=lambda: collections.deque(maxlen=DEVICE_MSG_HISTORY))
    con_received: int      = 0
    con_first_try: int     = 0
    retransmit_count: int  = 0
    inject_con_sent: int   = 0
    inject_con_acked: int  = 0
    seen_msg_ids: dict     = dataclasses.field(default_factory=dict)
    retransmit_chains: dict = dataclasses.field(default_factory=dict)  # msgId -> first_ts

@dataclasses.dataclass
class Alert:
    id: str
    msg: str
    ts: float
    kind: str    # "state" | "event"

@dataclasses.dataclass
class PendingInject:
    buf: bytes
    ip: str
    port: int
    msg_id: int
    sent_at: float
    retries: int
    topic: str
    device_name: str

def device_to_dict(d: Device) -> dict:
    now = time.time()
    chains = sum(1 for ts in d.retransmit_chains.values() if now - ts < CON_CHAIN_EXPIRE)
    return {
        "name": d.name,
        "ip": d.ip,
        "port": d.port,
        "last_seen_ago": round(now - d.last_seen, 1),
        "first_seen_ago": round(now - d.first_seen),
        "packet_count": d.packet_count,
        "publishes": d.publishes,
        "receives": d.receives,
        "messages": [
            {"ts_ago": round(now - m.ts, 1), "topic": m.topic,
             "decoded": m.decoded, "msg_type": m.msg_type}
            for m in d.messages
        ],
        "con_received": d.con_received,
        "con_first_try": d.con_first_try,
        "retransmit_count": d.retransmit_count,
        "inject_con_sent": d.inject_con_sent,
        "inject_con_acked": d.inject_con_acked,
        "active_retransmit_chains": chains,
    }

def alert_to_dict(a: Alert) -> dict:
    return {"id": a.id, "msg": a.msg, "ts": a.ts, "kind": a.kind}

# ── network state ─────────────────────────────────────────────────────────────

class NetworkState:
    def __init__(self, port: int, name: str, max_pending: int):
        self.port        = port
        self.name        = name
        self.max_pending = max_pending
        self.start_time  = time.time()
        self.join_active = False
        self._msg_id     = random.randint(1, 0xFFFF)

        self.devices: dict[str, Device]  = {}
        self.packet_ring: collections.deque = collections.deque(maxlen=PACKET_RING_SIZE)
        self.packet_times: collections.deque = collections.deque()
        self.alerts: dict[str, Alert]    = {}
        self.total_packets               = 0
        self.last_discovery_ts           = time.time()

        self.clients: set                = set()
        self._pending_inject: list[PendingInject] = []
        self._seen_con: collections.deque = collections.deque(maxlen=64)
        self._transport                  = None

    def next_msg_id(self) -> int:
        mid = self._msg_id
        self._msg_id = (self._msg_id % 0xFFFF) + 1
        return mid

    # ── device lookup ─────────────────────────────────────────────────────────

    def _get_or_create(self, name: str, ip: str, port: int) -> Device:
        if name not in self.devices:
            self.devices[name] = Device(
                name=name, ip=ip, port=port,
                first_seen=time.time(), last_seen=time.time())
        else:
            d = self.devices[name]
            d.ip, d.port = ip, port
        return self.devices[name]

    def _find_by_ip(self, ip: str) -> Optional[Device]:
        for d in self.devices.values():
            if d.ip == ip:
                return d
        return None

    # ── WebSocket broadcast ───────────────────────────────────────────────────

    async def broadcast(self, msg: dict):
        if not self.clients:
            return
        data = json.dumps(msg)
        dead = set()
        for ws in self.clients:
            try:
                await ws.send(data)
            except Exception:
                dead.add(ws)
        self.clients -= dead

    def full_state(self) -> dict:
        now = time.time()
        return {
            "type": "init",
            "uptime": round(now - self.start_time),
            "port": self.port,
            "monitor_name": self.name,
            "join_active": self.join_active,
            "max_pending": self.max_pending,
            "stats": self._stats(),
            "devices": {n: device_to_dict(d) for n, d in self.devices.items()},
            "packets": list(self.packet_ring),
            "alerts": [alert_to_dict(a) for a in self.alerts.values()],
        }

    def _stats(self) -> dict:
        now = time.time()
        cutoff = now - RATE_WINDOW
        while self.packet_times and self.packet_times[0] < cutoff:
            self.packet_times.popleft()
        rate = len(self.packet_times) / RATE_WINDOW

        total_inj_sent = sum(d.inject_con_sent  for d in self.devices.values())
        total_inj_ack  = sum(d.inject_con_acked for d in self.devices.values())
        rate_pct = round(total_inj_ack / total_inj_sent * 100) if total_inj_sent else None

        return {
            "uptime":          round(now - self.start_time),
            "active_devices":  len(self.devices),
            "total_packets":   self.total_packets,
            "packet_rate":     round(rate, 1),
            "con_success_rate": rate_pct,
        }

    # ── main packet handler ───────────────────────────────────────────────────

    async def handle_packet(self, data: bytes, src_ip: str, src_port: int):
        now = time.time()
        self.total_packets += 1
        self.packet_times.append(now)

        if not data:
            return

        if data[0] == DISC_MAGIC:
            await self._handle_discovery(data, src_ip, src_port, now)
        elif ((data[0] >> 6) & 0x03) == COAP_VER:
            await self._handle_coap(data, src_ip, src_port, now)

    async def _handle_discovery(self, data: bytes, src_ip: str, src_port: int, now: float):
        pkt = parse_discovery(data)
        if pkt is None or pkt["name"] == self.name:
            return

        real_name = pkt["name"]
        placeholder = f"?{src_ip}"
        if real_name not in self.devices and placeholder in self.devices:
            d = self.devices.pop(placeholder)
            d.name = real_name
            self.devices[real_name] = d
            await self.broadcast({"type": "device_removed", "name": placeholder})

        device = self._get_or_create(real_name, src_ip, pkt["port"])
        device.last_seen = now
        device.last_announce = now
        self.last_discovery_ts = now

        if pkt["disc_type"] == "probe" and self._transport:
            ann = build_discovery(DISC_ANNOUNCE, self.name, self.port)
            self._transport.sendto(ann, (src_ip, self.port))

        rec = {
            "ts": now, "src_ip": src_ip, "src_port": src_port,
            "kind": "discovery", "disc_type": pkt["disc_type"],
            "name": pkt["name"], "port": pkt["port"],
        }
        self.packet_ring.append(rec)

        await self._clear_state_alert("no_discovery")
        await self._clear_state_alert(f"timeout_risk_{pkt['name']}")
        await self._check_peer_table_alert()

        await self.broadcast({"type": "packet", "p": rec})
        await self.broadcast({"type": "device", "d": device_to_dict(device)})

    async def _handle_coap(self, data: bytes, src_ip: str, src_port: int, now: float):
        pkt = parse_coap(data)
        if pkt is None:
            return

        if pkt["kind"] == "ack":
            await self._handle_inject_ack(pkt["msg_id"], src_ip)
            return

        # Send ACK for CON before anything else
        is_retransmit = False
        if pkt["msg_type"] == "CON":
            if self._transport:
                self._transport.sendto(build_ack(pkt["msg_id"]), (src_ip, src_port))
            key = (src_ip, pkt["msg_id"])
            seen_list = list(self._seen_con)
            if key in seen_list:
                is_retransmit = True
            else:
                self._seen_con.append(key)

        device = self._find_by_ip(src_ip)
        if device is None:
            device = self._get_or_create(f"?{src_ip}", src_ip, src_port)
        device.last_seen = now
        device.packet_count += 1

        decoded = decode_payload(pkt["topic"], pkt["payload"])

        if not is_retransmit:
            device.publishes[pkt["topic"]] = decoded
            device.messages.appendleft(
                DeviceMsg(ts=now, topic=pkt["topic"], decoded=decoded, msg_type=pkt["msg_type"]))
            if pkt["msg_type"] == "CON":
                device.con_received += 1
                if pkt["msg_id"] not in device.seen_msg_ids:
                    device.con_first_try += 1
                device.seen_msg_ids[pkt["msg_id"]] = device.seen_msg_ids.get(pkt["msg_id"], 0) + 1
            # Update inferred "receives~" for all OTHER devices
            for name, other in self.devices.items():
                if other.ip != src_ip:
                    other.receives[pkt["topic"]] = decoded
        else:
            device.retransmit_count += 1
            device.retransmit_chains.setdefault(pkt["msg_id"], now)

        # Alerts
        await self._check_alerts(device, pkt, is_retransmit, now)

        if pkt["payload_truncated"]:
            await self._add_event_alert(
                f"payload_trunc_{int(now)}_{device.name}",
                f"Payload truncated ({device.name} → {pkt['topic']}, {len(pkt['payload'])}B)")

        rec = {
            "ts": now, "src_ip": src_ip, "src_port": src_port,
            "kind": "coap", "msg_type": pkt["msg_type"], "msg_id": pkt["msg_id"],
            "topic": pkt["topic"], "decoded": decoded,
            "payload_len": len(pkt["payload"]),
            "is_retransmit": is_retransmit,
            "device_name": device.name,
        }
        self.packet_ring.append(rec)
        await self.broadcast({"type": "packet", "p": rec})
        await self.broadcast({"type": "device", "d": device_to_dict(device)})

    # ── alert helpers ─────────────────────────────────────────────────────────

    async def _add_state_alert(self, alert_id: str, msg: str):
        if alert_id not in self.alerts:
            a = Alert(id=alert_id, msg=msg, ts=time.time(), kind="state")
            self.alerts[alert_id] = a
            await self.broadcast({"type": "alert_add", "alert": alert_to_dict(a)})

    async def _clear_state_alert(self, alert_id: str):
        if alert_id in self.alerts:
            del self.alerts[alert_id]
            await self.broadcast({"type": "alert_clear", "alert_id": alert_id})

    async def _add_event_alert(self, alert_id: str, msg: str):
        if alert_id not in self.alerts:
            a = Alert(id=alert_id, msg=msg, ts=time.time(), kind="event")
            self.alerts[alert_id] = a
            await self.broadcast({"type": "alert_add", "alert": alert_to_dict(a)})

    async def _check_alerts(self, device: Device, pkt: dict, is_retransmit: bool, now: float):
        # CON queue overflow [inferred]: simultaneous active retransmit chains > max_pending
        active_chains = sum(
            1 for ts in device.retransmit_chains.values()
            if now - ts < CON_CHAIN_EXPIRE)
        aid = f"con_overflow_{device.name}"
        if active_chains > self.max_pending:
            await self._add_state_alert(
                aid, f"CON queue overflow [inferred] ({device.name})")
        else:
            await self._clear_state_alert(aid)

    async def _check_peer_table_alert(self):
        n = len(self.devices)
        if n >= 6:
            await self._add_state_alert(
                "peer_table_full", f"Peer table full ({n}/6 peers)")
        else:
            await self._clear_state_alert("peer_table_full")

    # ── INJECT CON ACK handling ───────────────────────────────────────────────

    async def _handle_inject_ack(self, msg_id: int, src_ip: str):
        for p in self._pending_inject:
            if p.msg_id == msg_id and p.ip == src_ip:
                d = self._find_by_ip(src_ip)
                if d:
                    d.inject_con_acked += 1
                self._pending_inject.remove(p)
                now = time.time()
                rec = {
                    "ts": now, "src_ip": src_ip, "src_port": p.port,
                    "kind": "ack", "direction": "rx",
                    "msg_id": msg_id, "topic": p.topic,
                    "device_name": p.device_name,
                }
                self.packet_ring.append(rec)
                await self.broadcast({"type": "packet", "p": rec})
                if d:
                    await self.broadcast({"type": "device", "d": device_to_dict(d)})
                return

    # ── periodic tasks ────────────────────────────────────────────────────────

    async def tick_stats(self):
        while True:
            await asyncio.sleep(STATS_INTERVAL)
            now = time.time()

            # Peer timeout risk / cleanup
            for name, d in list(self.devices.items()):
                age = now - d.last_announce if d.last_announce else now - d.first_seen
                if age > PEER_TIMEOUT:
                    # Device timed out — remove from tracking
                    del self.devices[name]
                    await self._clear_state_alert(f"timeout_risk_{name}")
                    await self.broadcast({"type": "device_removed", "name": name})
                elif age > PEER_TIMEOUT_WARN:
                    await self._add_state_alert(
                        f"timeout_risk_{name}",
                        f"Peer timeout risk ({name}, last: {int(age)}s ago)")

            # No discovery traffic
            since_disc = now - self.last_discovery_ts
            if since_disc > DISCOVERY_ALERT_TIMEOUT:
                await self._add_state_alert(
                    "no_discovery", "No discovery traffic (broadcast blocked?)")

            await self.broadcast({"type": "stats", "stats": self._stats()})

    async def tick_join(self):
        while True:
            await asyncio.sleep(ANNOUNCE_INTERVAL)
            if self._transport:
                ann = build_discovery(DISC_ANNOUNCE, self.name, self.port)
                self._transport.sendto(ann, ("255.255.255.255", self.port))

    async def tick_inject_retry(self):
        while True:
            await asyncio.sleep(0.5)
            now = time.time()
            for p in list(self._pending_inject):
                if now - p.sent_at < CON_TIMEOUT:
                    continue
                if p.retries >= CON_MAX_RETRIES:
                    self._pending_inject.remove(p)
                    await self._add_event_alert(
                        f"con_lost_{int(now)}_{p.device_name}_{p.topic.lstrip('/')}",
                        f"CON lost ({p.device_name} → {p.topic})")
                    d = self._find_by_ip(p.ip)
                    if d:
                        await self.broadcast({"type": "device", "d": device_to_dict(d)})
                else:
                    p.retries += 1
                    p.sent_at = now
                    if self._transport:
                        self._transport.sendto(p.buf, (p.ip, p.port))

    # ── INJECT ────────────────────────────────────────────────────────────────

    async def do_inject(self, msg: dict):
        target_ip   = msg.get("target_ip", "")
        target_port = int(msg.get("target_port", self.port))
        topic       = msg.get("topic", "/")
        value_type  = msg.get("value_type", "raw")
        value_str   = msg.get("value", "")
        msg_type    = msg.get("msg_type", "NON")
        device_name = msg.get("target_name", target_ip)

        payload = encode_inject_value(value_type, value_str)
        mid = self.next_msg_id()
        buf = build_coap(topic, payload, msg_type, mid)

        if not self._transport:
            return

        self._transport.sendto(buf, (target_ip, target_port))

        d = self._find_by_ip(target_ip)
        if d is None:
            d = self._get_or_create(device_name, target_ip, target_port)
        d.inject_con_sent += (1 if msg_type == "CON" else 0)

        decoded = decode_payload(topic, payload)
        now = time.time()
        rec = {
            "ts": now, "src_ip": "monitor", "src_port": self.port,
            "kind": "inject", "direction": "tx",
            "msg_type": msg_type, "msg_id": mid,
            "topic": topic, "decoded": decoded,
            "payload_len": len(payload),
            "device_name": device_name,
            "target_ip": target_ip, "target_port": target_port,
        }
        self.packet_ring.append(rec)
        await self.broadcast({"type": "packet", "p": rec})

        if msg_type == "CON":
            self._pending_inject.append(PendingInject(
                buf=buf, ip=target_ip, port=target_port,
                msg_id=mid, sent_at=now, retries=0,
                topic=topic, device_name=device_name))

        await self.broadcast({"type": "device", "d": device_to_dict(d)})

    # ── JOIN NETWORK toggle ───────────────────────────────────────────────────

    async def set_join(self, active: bool):
        self.join_active = active
        if active and self._transport:
            probe = build_discovery(DISC_PROBE, self.name, self.port)
            self._transport.sendto(probe, ("255.255.255.255", self.port))
        await self.broadcast({"type": "join_state", "active": active})

    # ── export ────────────────────────────────────────────────────────────────

    def export_json(self) -> dict:
        now = time.time()
        return {
            "exported_at": now,
            "uptime": round(now - self.start_time),
            "port": self.port,
            "monitor_name": self.name,
            "devices": {n: device_to_dict(d) for n, d in self.devices.items()},
            "packets": list(self.packet_ring),
            "alerts": [alert_to_dict(a) for a in self.alerts.values()],
            "stats": self._stats(),
        }

# ── asyncio UDP protocol ──────────────────────────────────────────────────────

class UDPProtocol(asyncio.DatagramProtocol):
    def __init__(self, state: NetworkState):
        self.state = state

    def connection_made(self, transport):
        sock = transport.get_extra_info("socket")
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.state._transport = transport
        # Send a one-shot PROBE on startup so already-running devices reply immediately.
        # They will add the monitor as a temporary peer (times out after 95 s if JOIN stays OFF).
        probe = build_discovery(DISC_PROBE, self.state.name, self.state.port)
        transport.sendto(probe, ("255.255.255.255", self.state.port))

    def datagram_received(self, data: bytes, addr):
        src_ip, src_port = addr[0], addr[1]
        asyncio.ensure_future(self.state.handle_packet(data, src_ip, src_port))

    def error_received(self, exc):
        pass

# ── WebSocket handler ─────────────────────────────────────────────────────────

async def ws_handler(websocket, state: NetworkState):
    state.clients.add(websocket)
    try:
        await websocket.send(json.dumps(state.full_state()))
        async for raw in websocket:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            try:
                t = msg.get("type")
                if t == "inject":
                    await state.do_inject(msg)
                elif t == "join_toggle":
                    await state.set_join(bool(msg.get("active", False)))
                elif t == "dismiss_alert":
                    aid = msg.get("alert_id", "")
                    if aid in state.alerts:
                        del state.alerts[aid]
                        await state.broadcast({"type": "alert_clear", "alert_id": aid})
                elif t == "export_request":
                    await websocket.send(json.dumps({
                        "type": "export_data",
                        "data": state.export_json(),
                    }))
            except Exception as e:
                print(f"ws handler error: {e}")
    finally:
        state.clients.discard(websocket)

# ── HTTP + WebSocket server ───────────────────────────────────────────────────

def make_http_handler(state: NetworkState):
    html_path = Path(__file__).parent / "monitor.html"

    async def process_request(connection, request):
        upgrade = request.headers.get("Upgrade", "").lower()
        if upgrade == "websocket":
            return None  # let websockets handle the upgrade
        if request.path in ("/", "/index.html"):
            body = html_path.read_bytes()
            headers = Headers([
                ("Content-Type", "text/html; charset=utf-8"),
                ("Content-Length", str(len(body))),
            ])
            return WsResponse(200, "OK", headers, body)
        return None

    async def handler(websocket):
        await ws_handler(websocket, state)

    return handler, process_request

# ── main ──────────────────────────────────────────────────────────────────────

async def run(args):
    state = NetworkState(
        port=args.port,
        name=args.name,
        max_pending=args.max_pending)

    # UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", args.port))

    loop = asyncio.get_event_loop()
    await loop.create_datagram_endpoint(lambda: UDPProtocol(state), sock=sock)

    handler, process_request = make_http_handler(state)

    url = f"http://localhost:{args.http_port}"
    print(f"TrxNet Monitor  UDP:{args.port}  HTTP:{args.http_port}  name:{args.name}")
    print(f"Opening {url}")

    async with websockets.serve(
        handler, "localhost", args.http_port,
        process_request=process_request,
    ):
        webbrowser.open(url)
        await asyncio.gather(
            state.tick_stats(),
            state.tick_join(),
            state.tick_inject_retry(),
        )

def main():
    p = argparse.ArgumentParser(description="TrxNet Monitor")
    p.add_argument("--port",        type=int, default=5683,    help="UDP listen port (default 5683)")
    p.add_argument("--http-port",   type=int, default=8080,    help="HTTP/WS port (default 8080)")
    p.add_argument("--name",        type=str, default="MON.01",help="Monitor device name")
    p.add_argument("--max-pending", type=int, default=2,       help="TRXNET_MAX_PENDING value for overflow detection")
    args = p.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nStopped.")

if __name__ == "__main__":
    main()
