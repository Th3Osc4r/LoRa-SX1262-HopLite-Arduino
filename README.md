# Hoplite — Documentation & API Reference

**Version:** 1.0  
**Date:** February 2026  
**License:** MIT  
**Platform:** Arduino / ESP32 (S3 / WROOM / Heltec V3)  
**Depends on:** SX1262 LoRa Driver Suite v2.0+

---

## 1. Overview

Hoplite is a lightweight, embeddable mesh networking layer for LoRa point-to-point radios.  It operates on top of the SX1262 Arduino driver suite, adding multi-hop relay capability to any existing P2P deployment without taking over the firmware.

Hoplite has been stress-tested for 19+ hours across multiple hardware configurations at loads up to 10× production, achieving 99.8% alarm delivery (512/513 events) with zero crashes.

Hoplite uses **managed flooding** — packets propagate through every available relay rather than following computed routes.  There are no routing tables, no neighbor discovery messages, and no control traffic.  This makes the mesh simple to deploy, resilient to individual node failures, and deterministic in behavior.

### 1.1 Architecture

```
+------------------+
| Your Application |  ← You write this
+------------------+
        |
+------------------+
| Hoplite          |  ← Mesh layer (this library)
+------------------+
        |
+------------------+
| SX1262 Driver    |  ← Radio driver (separate library)
+------------------+
        |
+------------------+
| SX1262 Hardware  |  ← Physical radio chip
+------------------+
```

Hoplite never touches the radio directly.  All hardware interaction goes through the SX1262 driver's public API.  The coupling between Hoplite and the driver lives entirely in `hoplite_platform_bindings.cpp` — ten static functions that can be rewritten to target a different hardware platform (MCU, board, or RTOS) without modifying the core mesh logic.

### 1.2 Design Principles

- **No dynamic memory.**  All buffers are statically allocated at compile time.
- **No background threads.**  All processing is driven by explicit calls from your `loop()`.
- **No hidden transmissions.**  Hoplite never transmits without your code initiating it.
- **Deterministic execution.**  Every function has bounded execution time.
- **Regulatory compliance.**  Duty cycle enforcement is checked before every transmission.

### 1.3 What Hoplite Provides

- Managed-flooding mesh with configurable TTL (up to 15 hops)
- Packet deduplication via circular cache
- Relay scheduling with randomized jitter (prevents broadcast storms)
- Relay stamp tracking (5th header byte identifies last relay)
- Duty cycle awareness on both originated and relayed packets
- Relay deferral under duty cycle exhaustion (with configurable drop threshold)
- Optional AES-128-CCM authenticated encryption for addressed packets (broadcasts are always plaintext)
- Optional link health integration for passive relay monitoring
- Diagnostic statistics for every stage of the TX/RX/relay pipeline
- Compile-time feature toggles for minimal footprint

### 1.4 What Hoplite Does Not Provide

- Routing (no AODV, no DSR, no routing tables)
- Acknowledgments or delivery confirmation (see note below)
- Fragmentation or reassembly
- Dynamic address assignment
- Channel hopping or frequency management
- Application-layer protocols

These are deliberate exclusions.  Hoplite is a network layer, not an application framework.

**Why no acknowledgments:** The SX1262 is a half-duplex radio — it cannot transmit and receive at the same time.  Every ACK forces a TX→RX→TX state transition during which the node is completely deaf to other traffic.  In a mesh, the problem compounds: an ACK for a multi-hop packet must flood back through every relay, doubling on-air traffic, consuming duty cycle budget, and increasing collision probability.  When an ACK is lost, the sender cannot distinguish "data lost" from "ACK lost" — so it retransmits the data, adding duplicate packets to the channel.  More retransmissions mean more collisions, more lost ACKs, and more retransmissions — a feedback loop that worsens under load.  ACKs are architecturally incompatible with LoRa mesh networking at any meaningful node density or traffic rate.

Hoplite uses a different strategy: redundant transmission for critical events (3 copies per alarm, spaced in time) and acceptance of per-packet loss for routine telemetry.  This approach was validated across 513 alarm events with 99.8% event delivery — without a single acknowledgment packet.

---

## 2. Getting Started

### 2.1 Prerequisites

Before using Hoplite, you need a working SX1262 driver installation:

1. The SX1262 Arduino driver suite must be compiled and tested
2. The driver must be initialized (`sx1262_init_simple()` or `sx1262_init_extended()`)
3. The duty cycle manager must be initialized (`sx1262_dc_init()`)
4. (Optional) The security module must be initialized (`sx1262_security_init()`)
5. (Optional) The link health module must be initialized (`sx1262_link_health_init()`)

### 2.2 Files

Add these three files to your project alongside the SX1262 driver:

| File | Purpose |
|:-----|:--------|
| `hoplite.h` | Public API header — include this in your application |
| `hoplite.cpp` | Core mesh logic (platform-agnostic) |
| `hoplite_platform_bindings.cpp` | Arduino SX1262 driver bindings (compile-time included by `hoplite.cpp`) |

**Important:** Do not compile `hoplite_platform_bindings.cpp` separately.  It is `#include`d at the bottom of `hoplite.cpp` to allow the binding functions to be `static` and inlineable.

### 2.3 Validated Test Configuration

The following configuration was used during stress testing (19+ hours, 99.8% alarm delivery):

| Parameter | Value |
|:----------|:------|
| Frequency | 868.1 MHz (EU868) |
| Modulation | LoRa SF7 / BW125 |
| TX Power | +14 dBm |
| Duty Cycle Region | ETSI g1 (1%) |
| Default TTL | 7 |
| Relay Delay | 50–200 ms |
| Dedup Cache Size | 16 entries |
| Relay Queue Size | 4 entries |
| Hardware | ESP32-S3, Heltec V3, ESP32-WROOM + Waveshare SX1262 |

### 2.4 Minimal Example — Endpoint Node

```cpp
#include "hoplite.h"

// Callback: called when a mesh packet arrives for this node
void on_mesh_receive(const uint8_t *payload, uint8_t length,
                     const hoplite_rx_meta_t *meta) {
    Serial.printf("From node 0x%02X: ", meta->origin_id);
    Serial.write(payload, length);
    Serial.println();
}

void setup() {
    Serial.begin(115200);

    // ... SX1262 driver initialization here ...
    // ... Duty cycle init here ...

    // Configure Hoplite
    hoplite_config_t mesh_cfg;
    memset(&mesh_cfg, 0, sizeof(mesh_cfg));

    mesh_cfg.node_id           = 0x01;                  // This node's address
    mesh_cfg.role              = HOPLITE_ROLE_ENDPOINT;  // Send/receive only
    mesh_cfg.default_ttl       = 7;                      // Max 7 hops
    mesh_cfg.relay_delay_min_ms = 50;                    // Not used for endpoints
    mesh_cfg.relay_delay_max_ms = 200;                   // Not used for endpoints
    mesh_cfg.dedup_cache_size  = 16;
    mesh_cfg.security_ctx      = NULL;                   // No encryption
    mesh_cfg.tx_jitter_max_ms  = 0;                      // No jitter

    hoplite_result_t result = hoplite_init(&mesh_cfg);
    if (result != HOPLITE_OK) {
        Serial.printf("Hoplite init failed: %d\n", result);
        while (1) { delay(1000); }
    }

    hoplite_register_deliver_callback(on_mesh_receive);
}

void loop() {
    // --- RX: check for incoming packets ---
    uint8_t rx_buf[256];
    sx1262_rx_result_t rx_result;

    if (sx1262_receive(rx_buf, sizeof(rx_buf), 100, &rx_result) == SX1262_OK) {
        // Build metadata from driver results
        hoplite_rx_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        meta.rssi_dbm  = rx_result.rssi;
        meta.snr_db    = rx_result.snr;
        meta.rx_time_ms = millis();

        hoplite_process_rx(rx_buf, rx_result.payload_length, &meta);
    }

    // --- TX: send sensor reading every 30 seconds ---
    static uint32_t last_tx = 0;
    if (millis() - last_tx >= 30000) {
        last_tx = millis();

        uint8_t sensor_data[] = { 0x01, 0x17, 0x03 };  // Your payload
        hoplite_send(HOPLITE_ADDR_GATEWAY, sensor_data, sizeof(sensor_data), 0);
    }

    // --- Tick: process relay queue (required even for endpoints) ---
    hoplite_tick(millis());
}
```

### 2.5 Minimal Example — Repeater Node

A repeater differs from an endpoint in exactly two lines:

```cpp
mesh_cfg.node_id = 0x10;                  // Different address
mesh_cfg.role    = HOPLITE_ROLE_REPEATER;  // Enables relay logic
```

The repeater still calls `hoplite_process_rx()` and `hoplite_tick()` in its loop.  When a packet arrives that is not addressed to this node, Hoplite automatically schedules it for relay after a random jitter delay.  The actual relay transmission happens inside `hoplite_tick()`.

### 2.6 Minimal Example — Gateway Node

```cpp
mesh_cfg.node_id = HOPLITE_ADDR_GATEWAY;   // 0xFF
mesh_cfg.role    = HOPLITE_ROLE_GATEWAY;    // Required for address 0xFF
```

Only the gateway may originate broadcast packets (destination `0x00`).  This is a protocol-level restriction — `hoplite_send()` returns `HOPLITE_ERR_ROLE_FORBIDDEN` if a non-gateway node attempts broadcast.

---

## 3. API Reference

### 3.1 Initialization

#### `hoplite_init()`

```cpp
hoplite_result_t hoplite_init(const hoplite_config_t *config);
```

Initialize mesh state.  Must be called after the SX1262 driver and duty cycle manager are initialized.

**Validation performed:**

| Check | Error Returned |
|:------|:---------------|
| `config` is NULL | `HOPLITE_ERR_INVALID_PARAM` |
| Already initialized | `HOPLITE_ERR_INVALID_STATE` |
| `default_ttl > 15` | `HOPLITE_ERR_INVALID_PARAM` |
| `relay_delay_min > relay_delay_max` | `HOPLITE_ERR_INVALID_PARAM` |
| `dedup_cache_size` is 0 or exceeds `HOPLITE_DEDUP_CACHE_SIZE` | `HOPLITE_ERR_INVALID_PARAM` |
| `node_id` is `0x00` (broadcast) | `HOPLITE_ERR_INVALID_PARAM` |
| `node_id` is `0xFF` but `role` is not `HOPLITE_ROLE_GATEWAY` | `HOPLITE_ERR_INVALID_PARAM` |

Returns `HOPLITE_OK` on success.

---

#### `hoplite_deinit()`

```cpp
void hoplite_deinit(void);
```

Reset all internal state.  Does not interact with the radio.  Safe to call followed by `hoplite_init()` with a new configuration — this is how runtime role changes work (e.g., standby relay activation).

Returns immediately if called from within a delivery callback.

---

#### `hoplite_register_deliver_callback()`

```cpp
void hoplite_register_deliver_callback(hoplite_deliver_cb_t cb);
```

Register the function called when a packet is delivered to this node.  Only one callback is supported; calling again replaces the previous one.

**Callback signature:**

```cpp
void my_callback(const uint8_t *payload, uint8_t length,
                 const hoplite_rx_meta_t *meta);
```

**Restrictions from within the callback:**

- Do NOT call `hoplite_send()` — returns `HOPLITE_ERR_INVALID_STATE`
- Do NOT call `hoplite_process_rx()` — returns `HOPLITE_ERR_INVALID_STATE`
- Keep processing brief to avoid blocking relay scheduling

---

### 3.2 TX / RX Operations

#### `hoplite_send()`

```cpp
hoplite_result_t hoplite_send(uint8_t dest_id, const uint8_t *payload,
                              uint8_t length, uint8_t flags);
```

Send a packet into the mesh.

| Parameter | Description |
|:----------|:------------|
| `dest_id` | Destination node (1–254 unicast, `0x00` broadcast, `0xFF` gateway) |
| `payload` | Pointer to payload buffer (caller-owned, must be valid during call) |
| `length` | Payload length in bytes (1 to `HOPLITE_MAX_PAYLOAD_LEN`) |
| `flags` | Optional flags (ORed with protocol defaults; normally pass `0`) |

**Return values:**

| Code | Meaning |
|:-----|:--------|
| `HOPLITE_OK` | Transmitted successfully |
| `HOPLITE_ERR_NOT_INITIALIZED` | `hoplite_init()` not called |
| `HOPLITE_ERR_INVALID_STATE` | Called from within delivery callback |
| `HOPLITE_ERR_INVALID_PARAM` | NULL payload, zero length, or length exceeds max |
| `HOPLITE_ERR_ROLE_FORBIDDEN` | Non-gateway node attempted broadcast |
| `HOPLITE_ERR_TX_DENIED` | Duty cycle denied transmission |
| `HOPLITE_ERR_ENCRYPT_FAIL` | Encryption failed (security context issue) |

**Encryption behavior:** When `security_ctx` is non-NULL, unicast packets are automatically encrypted.  Broadcast packets are never encrypted — all nodes in the network must be able to read them, including relays that forward without decrypting.

**TX jitter:** When `tx_jitter_max_ms > 0`, a random delay (0 to max) is applied before each originated transmission to reduce collision probability in synchronized multi-node deployments.

---

#### `hoplite_process_rx()`

```cpp
hoplite_result_t hoplite_process_rx(uint8_t *buffer, uint8_t length,
                                    const hoplite_rx_meta_t *meta);
```

Process a received raw packet.  This function performs all mesh logic in a single call: header parsing, deduplication, link health recording, relay scheduling, decryption, and local delivery.

**Call this exactly once per received packet.**  The buffer is modified in-place (TTL decrement, decryption).

| Parameter | Description |
|:----------|:------------|
| `buffer` | Raw received packet (header + payload).  Modified in-place. |
| `length` | Total received length in bytes |
| `meta` | RX metadata from the driver (RSSI, SNR, timestamp).  May be NULL. |

**Return values:**

| Code | Meaning |
|:-----|:--------|
| `HOPLITE_OK` | Processed successfully (delivered and/or scheduled for relay) |
| `HOPLITE_ERR_INVALID_PARAM` | NULL buffer, length < 5 (header size), or not initialized |
| `HOPLITE_ERR_INVALID_STATE` | Called from within delivery callback |
| `HOPLITE_ERR_TTL_EXPIRED` | Packet TTL was 0 — dropped |
| `HOPLITE_ERR_DUPLICATE_PKT` | Already seen this (origin, packet_id) — dropped |
| `HOPLITE_ERR_PROTOCOL_VIOLATION` | Encrypted broadcast detected — dropped |
| `HOPLITE_ERR_DECRYPT_FAIL` | Authentication/decryption failed — dropped |

---

#### `hoplite_tick()`

```cpp
void hoplite_tick(uint32_t now_ms);
```

Advance relay scheduling and internal timers.  Call this from `loop()` at a frequency matching or exceeding your relay delay range.

This is where deferred relays are actually transmitted.  For each pending relay whose scheduled time has arrived, `hoplite_tick()` checks the duty cycle and either transmits, defers (reschedule), or drops (exceeded deferral limit).

| Parameter | Description |
|:----------|:------------|
| `now_ms` | Current time from `millis()` |

**Must be called even on endpoint nodes** — the tick function is the only path that processes the relay queue, and endpoints may still need internal housekeeping.

---

### 3.3 Statistics

#### `hoplite_get_statistics()`

```cpp
void hoplite_get_statistics(hoplite_statistics_t *stats);
```

Copy current statistics to a caller-provided structure.  If `HOPLITE_ENABLE_STATISTICS` is 0, fills the structure with zeros.

#### `hoplite_reset_statistics()`

```cpp
void hoplite_reset_statistics(void);
```

Reset all counters to zero.

#### Statistics Fields

| Field | Category | Description |
|:------|:---------|:------------|
| `tx_originated` | TX | Total packets passed to `hoplite_send()` |
| `tx_succeeded` | TX | Successfully transmitted |
| `tx_duty_cycle_denied` | TX | Blocked by duty cycle |
| `tx_encrypt_failed` | TX | Encryption failure |
| `rx_received` | RX | Total packets passed to `hoplite_process_rx()` |
| `rx_delivered` | RX | Delivered to local callback |
| `rx_dropped_duplicate` | RX | Dropped as duplicate |
| `rx_dropped_ttl` | RX | Dropped (TTL expired) |
| `rx_dropped_protocol` | RX | Protocol violation (encrypted broadcast) |
| `rx_decrypt_failed` | RX | Authentication/decryption failure |
| `relay_scheduled` | Relay | Packets queued for relay |
| `relay_transmitted` | Relay | Successfully relayed |
| `relay_deferred` | Relay | Deferred due to duty cycle |
| `relay_dropped_duty` | Relay | Dropped after exceeding deferral limit |
| `relay_dropped_overflow` | Relay | Dropped due to relay queue overflow |
| `callback_reentry_blocked` | Safety | API call attempted from within callback |
| `link_health_recorded` | Health | Link health samples recorded |

---

## 4. Configuration Reference

### 4.1 `hoplite_config_t` Fields

| Field | Type | Range | Default | Description |
|:------|:-----|:------|:--------|:------------|
| `node_id` | `uint8_t` | 1–254 (or 0xFF for gateway) | — | This node's address |
| `role` | `hoplite_role_t` | ENDPOINT / REPEATER / GATEWAY | — | Node role |
| `default_ttl` | `uint8_t` | 1–15 | — | Initial TTL for originated packets |
| `relay_delay_min_ms` | `uint16_t` | 0–65535 | — | Minimum relay jitter (ms) |
| `relay_delay_max_ms` | `uint16_t` | ≥ min | — | Maximum relay jitter (ms) |
| `dedup_cache_size` | `uint8_t` | 1–`HOPLITE_DEDUP_CACHE_SIZE` | — | Seen-packet cache entries |
| `tx_jitter_max_ms` | `uint16_t` | 0 = disabled | 0 | Max TX jitter for originated packets |
| `security_ctx` | `void *` | NULL or initialized context | NULL | Encryption context pointer |
| `link_health_interval_ms` | `uint32_t` | 0 = use default (60s) | 0 | Expected packet interval per node |

### 4.2 Compile-Time Options

Define these before including `hoplite.h`:

| Define | Default | Description |
|:-------|:--------|:------------|
| `HOPLITE_MAX_RELAY_DEFERRALS` | 3 | Max duty-cycle deferrals before relay drop |
| `HOPLITE_RELAY_QUEUE_SIZE` | 4 | Number of simultaneous pending relays |
| `HOPLITE_DEDUP_CACHE_SIZE` | 16 | Maximum entries in deduplication cache |
| `HOPLITE_ENABLE_STATISTICS` | 1 | Set to 0 to remove statistics code entirely |
| `HOPLITE_ENABLE_LINK_HEALTH` | 0 | Set to 1 to enable link health integration |

### 4.3 Special Addresses

| Address | Name | Purpose |
|:--------|:-----|:--------|
| `0x00` | `HOPLITE_ADDR_BROADCAST` | Broadcast to all nodes |
| `0xFF` | `HOPLITE_ADDR_GATEWAY` | Reserved gateway address |
| `0x01`–`0xFE` | — | User-assignable node addresses |

### 4.4 Node Roles

| Role | Relay | Broadcast TX | Description |
|:-----|:------|:-------------|:------------|
| `HOPLITE_ROLE_ENDPOINT` | No | No | Battery-powered sensor nodes |
| `HOPLITE_ROLE_REPEATER` | Yes | No | Always-on relay nodes |
| `HOPLITE_ROLE_GATEWAY` | Yes | Yes | Data collection point, may broadcast |

---

## 5. Packet Format

### 5.1 On-Air Layout

```
+----------+----------+--------+-----------+---------------+---------+
| byte 0   | byte 1   | byte 2 | byte 3    | byte 4        | bytes 5+|
+----------+----------+--------+-----------+---------------+---------+
| packet_id| origin_id| dest_id| flags:ttl | last_relay_id | payload |
+----------+----------+--------+-----------+---------------+---------+
```

**Total header: 5 bytes.**  Maximum on-air packet: 5 + 240 + 8 (security overhead) = 253 bytes.

### 5.2 Header Fields

| Field | Bits | Description |
|:------|:-----|:------------|
| `packet_id` | 8 | Sender-local sequence number (wraps at 255) |
| `origin_id` | 8 | Original sender's node ID |
| `dest_id` | 8 | Destination (0x00 = broadcast, 0xFF = gateway) |
| `flags` | 4 (upper nibble of byte 3) | Bit 4: encrypted flag. Bits 5–7: reserved |
| `ttl` | 4 (lower nibble of byte 3) | Remaining hop count |
| `last_relay_id` | 8 | Node ID of last relay (0x00 = direct from origin) |

### 5.3 Relay Stamp

The `last_relay_id` field (byte 4) is automatically updated by each relay node before retransmission.  This enables:

- **Path tracing** — the receiver knows which relay delivered the packet
- **Passive relay health monitoring** — a standby relay can watch for the primary relay's ID in passing traffic to confirm it is operational, with zero control overhead

### 5.4 Encryption Boundary

When `security_ctx` is provided and the destination is unicast (non-broadcast):

- The 5-byte header remains **unencrypted** (relays must read TTL and destination)
- The payload is encrypted in-place with AES-128-CCM
- Security overhead (4-byte counter + 4-byte MAC) is appended to the payload
- The `HOPLITE_FLAG_ENCRYPTED` bit is set in the flags nibble
- Broadcast packets are **never** encrypted — all nodes must be able to read them

---

## 6. Security Model

### 6.1 Overview

Hoplite's security is provided by the SX1262 driver suite's security module, which implements AES-128-CCM authenticated encryption.  Security is optional — when `security_ctx` is NULL, packets are transmitted in plaintext.  When enabled, it operates as a pre-shared key (PSK) model: all nodes in the network share the same 128-bit AES key, provisioned at deployment time.

This is the same trust model used by WPA2-Personal, LoRaWAN ABP activation, and most industrial sensor networks.

### 6.2 What Encryption Provides

When a unicast packet is transmitted with security enabled, three protections are applied:

**Confidentiality (AES-128-CCM encryption):** The payload is encrypted in-place.  Anyone intercepting the packet over the air — including relay nodes in the mesh path — sees only ciphertext.  The payload is opaque without the shared key.

**Integrity (4-byte MAC tag):** A message authentication code is computed over both the counter (as authenticated associated data) and the payload.  If any bit of the packet is modified in transit — by interference, injection, or tampering — the MAC verification fails and the gateway rejects the packet.  The probability of a successful forgery is approximately 1 in 4.3 billion per attempt, and each attempt requires a physical LoRa transmission on the correct frequency with the correct modulation parameters.

**Replay protection (monotonic counter with window):** Each packet carries a 4-byte counter that increments with every transmission.  The receiver tracks the last accepted counter and rejects any packet whose counter falls outside a configurable window (default: 32).  An attacker who records a valid packet off the air and rebroadcasts it later will have the correct MAC tag, but the counter will be rejected as stale.

If any check fails, `sx1262_secure_unpack()` returns an error code and zeros the receive buffer — no partial plaintext is ever exposed.

### 6.3 What Remains Visible

The 5-byte mesh header is **not encrypted**.  This is a deliberate design choice: relay nodes must read the destination address and TTL to make forwarding decisions.  Encrypting the header would require every relay to decrypt and re-encrypt every packet, which is incompatible with the zero-trust relay model (relays forward opaque payloads without possessing keys).

An attacker with a receiver on the correct frequency and modulation parameters can observe:

| Visible Field | What It Reveals |
|:--------------|:----------------|
| `origin_id` | Which node sent the packet |
| `dest_id` | Which node the packet is addressed to |
| `last_relay_id` | Which relay last forwarded it |
| `flags_ttl` | Whether the packet is encrypted, remaining hop count |
| `packet_id` | Sequence number (wrapping 0–255) |
| Packet timing | When each node transmits |
| Packet size | Approximate payload length |

This constitutes a **traffic analysis** exposure.  An attacker cannot read or forge your data, but can observe which nodes are active, how often they transmit, and the general topology of your network.

For most industrial sensor deployments, this is an acceptable trade-off.  The sensor data itself (temperatures, voltages, alarm states) is protected.  The fact that node `0x08` transmitted at 14:32 is typically not sensitive.  If your deployment requires traffic pattern confidentiality, LoRa's long on-air times and regulatory duty cycle constraints make traffic analysis mitigation impractical regardless of the mesh layer.

### 6.4 What Encryption Does Not Provide

**Per-node authentication:** The shared key authenticates the *network*, not individual nodes.  Any device with the key can claim any `origin_id`.  Within the trusted network, there is no cryptographic proof that a packet claiming to be from node `0x08` actually originated from that physical device.

For most deployments, this is not a practical concern — the attacker would need to obtain the key first, which requires either physical access to a device (extracting firmware or reading memory) or a side-channel attack.  At that point, they have compromised a device regardless of per-node identity.

**Forward secrecy:** If the shared key is compromised at any point, an attacker who recorded past encrypted traffic can decrypt it retroactively.  LoRa's low bandwidth and short payloads make this a low-value attack in practice — the cost of recording and decrypting years of soil moisture readings rarely justifies the effort.

**Broadcast encryption:** Broadcast packets (destination `0x00`) are never encrypted.  Broadcasts carry network-level messages (gateway commands) that all nodes must be able to read, including relays that forward without decrypting.  If you need encrypted group communication, use unicast to individual nodes.

### 6.5 No Over-the-Air Key Recovery

A common question is whether an attacker can force key material to appear on the air — the equivalent of a WiFi deauthentication attack, where forged management frames force a client to reconnect and expose the WPA2 4-way handshake for offline brute-force.

This attack does not apply.  WiFi deauth works because WPA2 must negotiate session keys over the air, and that negotiation leaks material that can be attacked offline.  Hoplite has no handshake — no key exchange, no session establishment, no authentication negotiation, no association or re-association.  The key is pre-shared at deployment time, flashed into firmware, and never transmitted over the air in any form.

Every packet is independently encrypted using the static PSK and a counter-derived nonce.  An attacker capturing encrypted packets gets ciphertext and the plaintext counter (used as AAD).  AES-128-CCM does not leak key material from ciphertext — there is no known practical attack that recovers an AES-128 key from captured ciphertext, regardless of how many packets are collected.

There is also no protocol message an attacker can forge to cause a node to reveal its key or reset its cryptographic state.  Nodes do not respond to unrecognized packets, do not negotiate parameters, and do not acknowledge failed decryptions.  A node that receives a packet with an invalid MAC simply drops it and increments the `auth_failures` counter.

The realistic attack vectors against the key are:

- **Physical access:** Extract the key from flash memory or RAM via JTAG, serial, or firmware dump.  Mitigated by enabling ESP32-S3 secure boot and flash encryption.
- **Side-channel:** Power analysis or electromagnetic emissions during AES operations.  Requires physical proximity and specialized equipment.
- **Brute force:** 2¹²⁸ key space — computationally infeasible with current or foreseeable technology.

All three require physical access to a device or its immediate electromagnetic environment.  No over-the-air attack path to the key exists.

### 6.6 Threat Assessment

| Threat | With Encryption | Without Encryption |
|:-------|:----------------|:-------------------|
| Eavesdropping (reading payload data) | **Blocked** — payload is AES-128-CCM encrypted | Exposed |
| Packet injection (fake sensor readings) | **Blocked** — MAC tag verification fails | Undetected |
| Packet modification in transit | **Blocked** — MAC tag verification fails | Undetected |
| Replay attack (rebroadcast captured packet) | **Blocked** — counter rejected as stale | Undetected |
| Over-the-air key recovery (deauth-style) | **Not applicable** — no handshake, no key material on air | N/A |
| Traffic analysis (who transmits when) | Visible — mesh header is plaintext | Visible |
| Node impersonation (within trusted network) | Possible if key is shared | Trivial |
| Key extraction from captured device | Possible via JTAG/firmware dump (see mitigations below) | N/A |

### 6.7 Operator Responsibilities

Security is a system property, not just a software feature.  The encryption module provides strong payload protection, but the overall security posture depends on how the deployment is managed.

**Key provisioning:** The 128-bit AES key must be pre-shared securely.  It should never be transmitted over LoRa, stored in version control, or hardcoded in publicly available firmware images.  Use a unique key per deployment (not the same key across all your customers' installations).

**Counter persistence:** The TX counter must survive reboots.  If a node resets its counter to zero after a power cycle, it creates a window where an attacker could replay previously captured packets with valid counters.  The security module supports automatic NVS persistence via `sx1262_security_init_with_storage()` — use it in production.

**Physical security:** An attacker with physical access to a node can extract the firmware and the key from flash memory.  On ESP32-S3, enabling **secure boot** (prevents unauthorized firmware) and **flash encryption** (prevents flash readout) closes this vector.  These are platform configurations, not driver features — consult the ESP-IDF security documentation.

**Key rotation:** If a node is lost, stolen, or suspected compromised, the shared key should be rotated across all remaining nodes.  This is an operational procedure, not an automated feature.  Plan for it in your deployment maintenance workflow.

**Monitoring:** The security module tracks `auth_failures` and `replay_rejections` in its statistics structure.  A non-zero `auth_failures` count at the gateway indicates someone is transmitting packets with incorrect keys on your frequency — either interference, misconfiguration, or an active attack.  Monitor this counter in production.

---

## 7. Relay Redundancy System

Hoplite's relay stamp (byte 4) enables a complete relay failover system at the application level with no additional protocol messages.

### 7.1 Concept

In a deployment with two relay nodes (Main and Standby) between endpoints and a gateway:

1. **Normal operation:** Main relays all packets.  Every relayed packet carries Main's node ID in the `last_relay_id` field.
2. **Standby monitors:** The Standby node watches incoming traffic.  When it sees Main's relay ID in passing packets, it knows Main is alive — no heartbeat needed.
3. **Main goes offline:** Standby stops seeing Main's relay ID.  After a configurable timeout (e.g., 2 minutes of silence), Standby activates itself as a repeater.
4. **Failover:** Standby calls `hoplite_deinit()` then `hoplite_init()` with `HOPLITE_ROLE_REPEATER`.  It now relays traffic and sends an alarm to the gateway.
5. **Main recovers:** Standby detects Main's relay ID reappearing.  It steps down by reinitializing as an endpoint.  It sends a recovery alarm.

### 7.2 Implementation Pattern

The relay alarm and failover logic is built at the **application level** using Hoplite's API — it is not part of the Hoplite core.  The Firmware Cookbook (companion document) contains validated, copy-paste-ready functions for:

| Component | Description |
|:----------|:------------|
| `send_relay_alarm()` | Sends 3 copies of OFFLINE/RECOVERED alarm to gateway |
| Standby relay health monitor | Tracks `last_relay_id` in `on_mesh_receive()` callback |
| Failover trigger | Timeout-based activation in `loop()` |
| Handback logic | Detects Main recovery and steps down |

This was validated end-to-end with 100% alarm delivery (6/6 copies received).  The alarm redundancy strategy was subsequently validated across 513 alarm events in 8 hours of stress testing at up to 4.5× production load, achieving a 99.8% event delivery rate with a consistent redundancy ratio of 2.7 copies per event.

---

## 8. Platform Bindings

### 8.1 Architecture

All coupling between Hoplite and the underlying radio lives in `hoplite_platform_bindings.cpp`.  This file implements 10 `static` functions:

| # | Function | Purpose | Arduino Implementation |
|:--|:---------|:--------|:----------------------|
| 1 | `hoplite_get_time_ms()` | Monotonic clock | `millis()` |
| 2 | `hoplite_delay_ms()` | Blocking delay | `delay()` |
| 3 | `hoplite_random_range()` | Random value in range | `esp_random()` hardware TRNG |
| 4 | `hoplite_driver_tx()` | Transmit packet | `sx1262_transmit()` |
| 5 | `hoplite_calculate_airtime()` | Time-on-air for packet | `sx1262_get_time_on_air_ms()` |
| 6 | `hoplite_dc_can_transmit()` | Duty cycle check | `sx1262_dc_can_transmit()` |
| 7 | `hoplite_dc_record_tx()` | Record TX airtime | `sx1262_dc_record_transmission()` |
| 8 | `hoplite_secure_pack()` | Encrypt payload | `sx1262_secure_pack()` |
| 9 | `hoplite_secure_unpack()` | Decrypt payload | `sx1262_secure_unpack()` |
| 10 | `hoplite_record_link_health()` | Record RX signal quality | `sx1262_link_health_record_rx_auto()` |

### 8.2 Porting to a Different Platform

To port Hoplite to a different hardware platform (different MCU, board, or RTOS):

1. Copy `hoplite_platform_bindings.cpp`
2. Rewrite each of the 10 functions to call your platform's equivalents
3. Keep the function signatures identical
4. The core (`hoplite.h` and `hoplite.cpp`) requires no changes

The airtime calculation binding deserves attention: the Arduino version caches the LoRa config struct on first call because `sx1262_get_time_on_air_ms()` requires it as a parameter.  If your platform provides a simpler ToA function that reads config internally, the cache is unnecessary.

---

## 9. Usage Patterns

### 9.1 Encryption with Hoplite

```cpp
// Before hoplite_init():
sx1262_security_context_t sec_ctx;
uint8_t key[16] = { /* your 128-bit key */ };
sx1262_security_init(&sec_ctx, key, 0);

// In config:
mesh_cfg.security_ctx = &sec_ctx;  // void* cast handled internally
```

All unicast packets are now encrypted.  Broadcasts remain plaintext.

### 9.2 Printing Statistics

```cpp
hoplite_statistics_t stats;
hoplite_get_statistics(&stats);

Serial.printf("TX: %lu originated, %lu succeeded, %lu denied\n",
              stats.tx_originated, stats.tx_succeeded,
              stats.tx_duty_cycle_denied);
Serial.printf("RX: %lu received, %lu delivered, %lu duplicates\n",
              stats.rx_received, stats.rx_delivered,
              stats.rx_dropped_duplicate);
Serial.printf("Relay: %lu scheduled, %lu transmitted, %lu deferred\n",
              stats.relay_scheduled, stats.relay_transmitted,
              stats.relay_deferred);
```

### 9.3 Runtime Role Change

```cpp
// Standby relay activates:
hoplite_deinit();
mesh_cfg.role = HOPLITE_ROLE_REPEATER;
hoplite_init(&mesh_cfg);

// ... later, main relay recovers, standby steps down:
hoplite_deinit();
mesh_cfg.role = HOPLITE_ROLE_ENDPOINT;
hoplite_init(&mesh_cfg);
```

### 9.4 Multiple Receive Contexts

The `hoplite_rx_meta_t` structure returned in the delivery callback contains everything needed to identify the packet's path:

```cpp
void on_receive(const uint8_t *payload, uint8_t len,
                const hoplite_rx_meta_t *meta) {
    Serial.printf("From: 0x%02X  Via: %s  RSSI: %d dBm  SNR: %d dB\n",
                  meta->origin_id,
                  meta->last_relay_id == 0x00 ? "direct" : "relay",
                  meta->rssi_dbm,
                  meta->snr_db);
}
```

---

## 10. Design Envelope

Hoplite is designed for networks of **5 to 75 nodes** in deployments where every node matters and reliability beats scale.  The managed flooding protocol is appropriate for industrial clusters, agricultural fields, building infrastructure, and similar constrained topologies.

### 10.1 Validated Capacity

Stress testing was conducted over 19+ hours on real hardware (ESP32-S3, Heltec V3, ESP32-WROOM with Waveshare SX1262) at 868.1 MHz EU868, SF7/BW125, under ETSI 1% duty cycle enforcement.

| Deployment Size | Confidence | Validation Basis |
|:----------------|:-----------|:-----------------|
| **50 nodes** (design target) | Very High | Baseline test: 91.6% stats, 100% alarms, 4 hours |
| **75 nodes** (recommended max) | High | 50% safety margin from validated 3× load |
| **100 nodes** | Moderate | Requires optimal RF conditions |
| **125+ nodes** | Low | Lab-only; not recommended for production |

**Delivery rate progression under increasing load:**

| Load | Equivalent Nodes | Stats Delivery | Alarm Delivery | System Stability |
|:-----|:-----------------|:---------------|:---------------|:-----------------|
| 1× | 50 | 91.6% | 100% | Zero crashes |
| 2× | ~100 | 97.0% | 100% | Zero crashes |
| 3× | ~150 | 92.9% | 100% | Zero crashes |
| 4.5× | ~225 | 91.3% | 99.7% | Zero crashes |
| 10× | ~540 | 64.4% | 94.2% | Zero crashes |

**A note on stats delivery rates:** LoRa is an unacknowledged, long-range radio protocol.  Every packet is a single fire-and-forget transmission with no physical-layer retry.  Per-packet loss of 5–15% is normal and expected due to multipath fading, ISM-band interference, and timing collisions — comparable to what LoRaWAN Class A devices achieve (85–95% typical).  A stats delivery rate of 91% at production load means 22 of every 24 hourly sensor readings arrive successfully, which is more than sufficient for trending and monitoring.  Critical alarms use 3-copy redundant transmission precisely because the medium is lossy — the probability of losing all 3 independent copies is approximately 0.07%, which is why alarm delivery measured 99.8% across 513 events.

The system was pushed to 10× production load (5-second TX intervals, equivalent to ~540 nodes) without a single crash, watchdog reset, or duty cycle violation.  The breaking point for alarm delivery was not found — even at 40% alarm probability with 4.5× load, the system maintained 100% per-phase delivery.

### 10.2 Limits

| Parameter | Limit | Reason |
|:----------|:------|:-------|
| Max nodes | 254 | 8-bit addressing (0x01–0xFE) |
| Max hops | 15 | 4-bit TTL field |
| Max payload | 240 bytes | LoRa packet size constraint |
| Relay queue | 4 (compile-time) | Static allocation, FIFO eviction |
| Dedup cache | 16 (compile-time) | Ring buffer, oldest evicted |
| Recommended max nodes | 75 | Validated with 50% real-world safety margin |

### 10.3 When Flooding Breaks Down

Managed flooding works well in sparse, low-traffic networks.  It degrades gracefully when overloaded:

- **High node density** with many active transmitters — relay storms consume airtime (stats delivery drops to ~65% at 10× load, but alarms still achieve 94%)
- **High traffic rates** — duty cycle exhaustion cascades through relays; the deferral mechanism handles this by rescheduling rather than dropping
- **Large networks** (>75 active nodes) — deduplication cache saturates, duplicates may leak through; increase `HOPLITE_DEDUP_CACHE_SIZE` for larger deployments
- **Weak RF links** — the single missed alarm in 513 events came from the node with the weakest signal (RSSI avg −88 dBm); physical deployment quality is the primary reliability factor

For deployments beyond 75 nodes, consider a routed protocol, reduce the TTL to limit flood radius, or segment into multiple independent mesh clusters.

---

## 11. Troubleshooting

### 11.1 Common Issues

| Symptom | Cause | Solution |
|:--------|:------|:---------|
| `hoplite_init()` returns `HOPLITE_ERR_INVALID_STATE` | Called twice without `hoplite_deinit()` | Call `hoplite_deinit()` first |
| `hoplite_send()` returns `HOPLITE_ERR_TX_DENIED` | Duty cycle exhaustion | Reduce TX rate or wait for budget to refill |
| `hoplite_send()` returns `HOPLITE_ERR_ROLE_FORBIDDEN` | Endpoint/repeater attempted broadcast | Only gateway nodes can broadcast |
| Packets not relayed | Node role is ENDPOINT | Set role to REPEATER or GATEWAY |
| Duplicate packets reaching callback | Dedup cache too small | Increase `HOPLITE_DEDUP_CACHE_SIZE` |
| Relay transmission timing out | Driver not initialized | Ensure SX1262 driver init before `hoplite_init()` |
| Statistics all zeros | `HOPLITE_ENABLE_STATISTICS` is 0 | Set to 1 before including `hoplite.h` |
| Airtime calculation returns 500ms | LoRa config cache failed | Ensure driver is fully initialized before Hoplite |
