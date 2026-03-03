# Hoplite — Engineering Overview

**Architecture, test results, trade-offs, and comparison with alternatives**

---

## What Hoplite Is

Hoplite is a C/C++ library (~500 lines of core logic) that adds managed-flooding mesh networking to SX1262 LoRa radio deployments.  It is compiled into existing firmware as three source files and accessed through a C API.  It does not own the main loop, does not configure the radio, and does not allocate dynamic memory.

Hoplite operates on top of the SX1262 Arduino driver suite.  All radio interaction goes through 10 static binding functions in a single file (`hoplite_platform_bindings.cpp`), which can be rewritten to target a different hardware platform (MCU, board, or RTOS) without modifying the core mesh logic.  Hoplite is specific to the SX1262 radio and its driver suite.

```
+------------------+
| Application      |  ← Your firmware
+------------------+
        |
+------------------+
| Hoplite          |  ← Mesh layer (this library)
+------------------+
        |
+------------------+
| SX1262 Driver    |  ← Radio driver
+------------------+
        |
+------------------+
| SX1262 Hardware  |
+------------------+
```

---

## Protocol Design

### Managed Flooding

Hoplite uses managed flooding.  Every relay-capable node retransmits every packet it hasn't seen before, with the TTL decremented.  There are no routing tables, no neighbor discovery, and no control traffic.

Properties:

- **Path redundancy is automatic.**  If multiple relays exist, a packet reaches its destination via every available path.  No single relay failure causes packet loss.
- **No convergence time.**  There is no routing state to rebuild after topology changes.  A node that powers on and begins relaying is immediately useful.
- **Traffic scales with relay count, not network diameter.**  Every relay retransmits every packet.  In a 10-relay network, one originated packet generates up to 10 relay transmissions.

The trade-off is airtime consumption.  Flooding is appropriate for networks with low to moderate traffic and sparse relay density.  It degrades under high node counts or high traffic rates because relay transmissions consume duty cycle budget and increase collision probability.

### Packet Format

5-byte header, no options, no extensions:

```
[packet_id:1][origin_id:1][dest_id:1][flags_ttl:1][last_relay_id:1][payload:N]
```

- `flags_ttl`: upper nibble is flags (bit 4 = encrypted), lower nibble is TTL (0–15)
- `last_relay_id`: updated by each relay before retransmission.  Enables passive relay health monitoring without control traffic.
- Maximum payload: 240 bytes.  With encryption overhead (4-byte counter + 4-byte MAC): 232 bytes effective.

### Addressing

8-bit static addresses, assigned at initialization.  Address `0x00` is broadcast (gateway-only privilege), address `0xFF` is reserved for the gateway.  This gives 254 usable node addresses.

There is no dynamic address assignment, no neighbor table, and no node discovery.  Addresses are configured by the operator at deployment time.

### Deduplication

A ring buffer of `(origin_id, packet_id)` pairs (default: 16 entries) tracks seen packets.  Duplicates are dropped silently.  The cache is FIFO — when full, the oldest entry is evicted.

### Relay Scheduling

Relays are not immediate.  Each packet is queued with a random delay drawn from a configurable range (default: 50–200 ms).  This jitter prevents multiple relays from transmitting simultaneously, which would cause collisions and waste all their airtime.

The relay queue holds 4 entries (compile-time configurable).  When full, the oldest pending relay is evicted to make room (FIFO policy — favors fresher data).

### Duty Cycle Integration

Before every transmission — originated or relayed — Hoplite queries the driver's duty cycle manager.  The behavior differs by packet origin:

- **Originated packets (`hoplite_send`):** If denied, the function returns `HOPLITE_ERR_TX_DENIED` immediately.  The application decides whether to retry.
- **Relayed packets (`hoplite_tick`):** If denied, the relay is rescheduled to `now + wait_time`.  After a configurable number of deferrals (default: 3), the relay is dropped.

This prevents relay nodes from silently violating duty cycle limits.

### Relay Stamp and Passive Health Monitoring

The `last_relay_id` field (byte 4) is written by each relay before retransmission.  The receiving node — whether the final destination or a standby relay — can read this field to determine which relay last handled the packet.

A standby relay can monitor this field passively.  If the primary relay's ID appears in passing traffic, the primary is operational.  If it disappears for a configurable timeout, the standby can activate itself by reinitializing with `HOPLITE_ROLE_REPEATER`.

The failover logic is implemented at the application level using Hoplite's public API, not inside the mesh core.  The complete failover sequence (primary offline → standby detects → activates → sends alarm → primary recovers → standby steps down) was validated with 100% alarm delivery (6/6 copies received).

---

## Why Not Acknowledgments

LoRa uses half-duplex radios.  The SX1262 cannot transmit and receive simultaneously.  This creates specific problems for ACK-based protocols:

**State transition cost:**  Every ACK requires a TX→RX→TX cycle.  During the RX window waiting for the ACK, the node is deaf to all other traffic.  In a network with multiple transmitters, this deafness window causes missed packets from other nodes.

**Mesh ACK amplification:**  An ACK for a multi-hop packet must traverse the mesh in reverse.  Each hop generates a relay transmission.  A 3-hop data packet followed by a 3-hop ACK generates 6 relay transmissions for one exchange — tripling the airtime of a send-and-forget model.

**Duty cycle consumption:**  Under ETSI EN 300 220, LoRa devices on most EU868 sub-bands must not exceed 1% duty cycle.  ACK traffic can consume more duty cycle budget than the data itself, reducing the node's capacity for actual sensor transmissions.

**Lost-ACK retransmission spiral:**  When an ACK is lost, the sender cannot distinguish "data lost" from "ACK lost."  It retransmits the data.  The receiver, having already received the original, sends another ACK for the duplicate.  Under congestion — precisely when reliability matters most — this creates a positive feedback loop: more retransmissions → more collisions → more lost ACKs → more retransmissions.

Hoplite's design response is redundant transmission for critical events: 3 copies per alarm, spaced 20 seconds apart, each an independent trial against the per-packet loss rate.  This achieved 99.8% event delivery across 513 events in stress testing, with no return traffic and no additional radio state transitions.

For routine telemetry, per-packet loss is accepted.  A sensor reading sent every 15 minutes that arrives 91% of the time produces 22 of 24 hourly readings — sufficient for trending, dashboards, and historical analysis.

---

## Test Results

Testing was conducted over 19+ hours on real hardware at 868.1 MHz EU868, SF7/BW125, under ETSI 1% duty cycle enforcement.

### Hardware

| Role | Hardware |
|:-----|:---------|
| Gateway | ESP32-S3 + Waveshare SX1262 |
| Relay | ESP32-WROOM + Waveshare SX1262 |
| Node 1 | Heltec WiFi LoRa 32 V3 |
| Node 2 | ESP32-S3 + Waveshare SX1262 |
| Node 3 | Heltec WiFi LoRa 32 V3 |

### Configuration

| Parameter | Value |
|:----------|:------|
| Frequency | 868.1 MHz |
| Modulation | SF7 / BW125 |
| TX Power | +14 dBm |
| Duty Cycle | ETSI g1 (1%) |
| Default TTL | 7 |
| Relay Delay | 50–200 ms |
| Dedup Cache | 16 entries |
| Relay Queue | 4 entries |
| Alarm Strategy | 3 copies, 20-second spacing |

### Delivery Rates

| Load | Equiv. Nodes | Stats Delivery | Alarm Delivery | Duration | Crashes |
|:-----|:-------------|:---------------|:---------------|:---------|:--------|
| 1× | 50 | 91.6% | 100% (16/16) | 4 hrs | 0 |
| 2× | ~100 | 97.0% | 100% (17/17) | 3.25 hrs | 0 |
| 3× | ~150 | 92.9% | 100% (215/215) | 4 hrs | 0 |
| 4.5× | ~225 | 91.3% | 99.7% (297/298) | 4 hrs | 0 |
| 10× | ~540 | 64.4% | 94.2% (65/69) | 1 hr | 0 |

### Context for Stats Delivery Rates

LoRa operates without acknowledgments or retries at the physical layer.  Every packet is a single unconfirmed transmission.  Per-packet loss of 5–15% is inherent to the medium, caused by multipath fading, ISM-band interference, and timing collisions.

For reference, LoRaWAN Class A considers 90% uplink delivery acceptable.  Real-world LoRaWAN deployments report 85–95% under normal conditions.  Commercial LoRa P2P systems (direct point-to-point, no mesh) achieve 90–98% depending on range and environment.

The 91% stats delivery at 1× load means 22 of every 24 hourly sensor readings arrive.  For telemetry applications, the next reading arrives on schedule regardless of whether the previous one was lost.

Alarm delivery is architecturally separate: 3-copy redundant transmission gives each alarm event three independent trials against the per-packet loss rate.  At 91% per-packet delivery, the probability of losing all 3 copies is approximately (1 − 0.91)³ ≈ 0.07%.  The measured result — 99.8% across 513 events — is consistent with this model.

### Key Findings

The alarm delivery breaking point was not found.  At 40% alarm probability with 4.5× production load, per-phase delivery remained at 100%.

The single missed alarm (1 of 513) occurred on the node with the weakest RF link (RSSI avg −88 dBm vs −83/−84 dBm for other nodes).  This indicates antenna placement and RF environment are the primary reliability factors, not protocol behavior.

Redundancy ratio remained stable at 2.7 copies per event across all load levels, from 1× to 4.5×.  Decorrelation (staggering first copies across nodes during burst alarms) provided no measurable benefit and is not recommended.

Zero watchdog resets, zero duty cycle violations, and zero crashes were recorded across all test phases.

### Capacity Assessment

| Deployment Size | Confidence | Basis |
|:----------------|:-----------|:------|
| ≤50 nodes | High | Baseline test: 91.6% stats, 100% alarms |
| 51–75 nodes | High | 50% safety margin from validated 3× load (150-equiv.) |
| 76–100 nodes | Moderate | Approaches observed limits; requires good RF environment |
| >100 nodes | Low | Lab-only; not recommended without field validation |

Recommended production limit: **75 nodes.**  This provides a 50% safety margin from the 3× load level where 100% alarm delivery was sustained over 4 hours.  Real-world conditions (longer range, multipath, interference, temperature variation) justify this derating from the 150-node-equivalent lab result.

---

## Security Model

### Pre-Shared Key (PSK) Architecture

Hoplite's encryption is provided by the SX1262 driver suite's security module.  It uses AES-128-CCM authenticated encryption with a pre-shared 128-bit key.  This is the same trust model as WPA2-Personal and LoRaWAN ABP activation.

Security is optional.  When `security_ctx` is NULL, packets are plaintext.

### Per-Packet Protection

Each encrypted unicast packet carries:

- **4-byte counter** (plaintext, used as authenticated associated data for nonce construction)
- **Encrypted payload** (AES-128-CCM)
- **4-byte MAC tag** (message authentication code)

On receive, three checks are performed sequentially:

1. **Counter validation** — must be within the replay window (default: 32) of the last accepted counter.  Rejects replayed packets.
2. **MAC verification** — proves the sender possessed the correct key and the packet was not modified.  Forgery probability: ~1 in 4.3 billion per attempt.
3. **Decryption** — produces plaintext only if both checks pass.

If any check fails, the receive buffer is zeroed.  No partial plaintext is exposed.  The security module returns a specific error code (`SX1262_SEC_ERROR_REPLAY`, `SX1262_SEC_ERROR_AUTH_FAILED`) for diagnostic purposes.

Broadcast packets (destination `0x00`) are never encrypted — all nodes must be able to read them, including relays that forward without decrypting.

### What Is Not Protected

**Mesh header:**  The 5-byte header is plaintext.  Relay nodes must read the destination and TTL to make forwarding decisions.  An attacker with a receiver on the correct frequency and modulation can observe: which nodes are active, how often they transmit, which relays handle traffic, and the general network topology.

**Per-node identity:**  The shared key authenticates the network, not individual nodes.  Any device with the key can claim any `origin_id`.  There is no cryptographic proof that a packet claiming to be from node `0x08` originated from that physical device.  This is an acceptable trade-off when physical access to deployed devices is controlled and the threat model is external attackers, not compromised insiders.

**Forward secrecy:**  If the shared key is compromised, previously recorded encrypted traffic can be decrypted retroactively.

### Operator Responsibilities

The encryption module provides the cryptographic mechanism.  The overall security posture depends on deployment practices:

- **Key provisioning:**  The key must be pre-shared securely — not over LoRa, not in version control, not hardcoded in public firmware images.  Use a unique key per deployment.
- **Counter persistence:**  The TX counter must survive reboots.  The security module supports automatic NVS persistence via `sx1262_security_init_with_storage()`.  Without this, a reboot resets the counter to zero and opens a replay window.
- **Physical security:**  On ESP32-S3, enable secure boot (prevents unauthorized firmware) and flash encryption (prevents flash readout).  These are ESP-IDF platform configurations, not driver features.
- **Key rotation:**  If a node is lost, stolen, or suspected compromised, rotate the key across all remaining nodes.
- **Monitoring:**  The security module tracks `auth_failures` and `replay_rejections`.  A non-zero `auth_failures` count at the gateway indicates someone is transmitting packets with incorrect keys on your frequency.

---

## Comparison with Alternatives

### Meshtastic

Meshtastic is firmware that turns a LoRa device into a node in the Meshtastic messaging network.  It includes companion apps for Android, iOS, and web.  Once flashed, Meshtastic owns the device — application code, radio configuration, sleep management, and user interface are all part of Meshtastic.

Hoplite is a library compiled into existing firmware.  The application retains control of the main loop, sensor hardware, sleep schedules, data formats, and radio configuration.

### MeshCore (Andy Kirby)

MeshCore is a LoRa mesh project with companion apps, a flashing tool, and active communities in Germany and the UK.  It uses state-aware mesh networking rather than stateless flooding.  Like Meshtastic, it is firmware that owns the device once flashed.

### Comparison Table

| Property | Hoplite | Meshtastic | MeshCore (Kirby) |
|:---------|:--------|:-----------|:-----------------|
| Type | Library (3 source files) | Firmware | Firmware |
| License | MIT | GPL | Proprietary / mixed |
| Application code | Preserved | Replaced | Replaced |
| Mesh protocol | Managed flooding | Flooding | State-aware |
| Addressing | 8-bit static (254 nodes) | Dynamic | Dynamic |
| Max hops | 15 (configurable) | 7 (default) | Varies |
| Encryption | AES-128-CCM per-packet (PSK) | AES-256 (channel-level) | End-to-end |
| Replay protection | Monotonic counter, window 32 | Channel-based | Yes |
| Duty cycle enforcement | Per-TX check, originated + relayed | Varies by config | Varies |
| Relay health monitoring | Passive via relay stamp | Community-built | Built-in |
| Node discovery | None (static assignment) | Automatic | Automatic |
| Phone app | None | Android, iOS, web | Android, iOS |
| Core code size | ~500 lines | ~100K+ lines | ~50K+ lines |
| Validated capacity | 75 nodes (tested to 225-equiv.) | Community-reported | Community-reported |

### When Meshtastic or MeshCore Is the Better Choice

- The application is person-to-person messaging and a phone app is required.
- Nodes must join and leave the network dynamically without operator intervention.
- A large existing community and ecosystem of compatible hardware is a requirement.

---

## Limitations

**Scalability ceiling.**  Managed flooding generates O(R) relay transmissions per originated packet, where R is the number of relay-capable nodes.  Beyond approximately 75 nodes, relay traffic saturates available airtime and deduplication caches overflow.  Deployments beyond this size require a routed protocol, TTL reduction to limit flood radius, or segmentation into independent mesh clusters.

**No guaranteed delivery.**  There is no per-packet delivery confirmation.  Alarm reliability is achieved through redundant transmission (see "Why Not Acknowledgments" above for the engineering rationale).  Applications requiring confirmed delivery of every packet should evaluate whether LoRa is the appropriate radio technology.

**Static addressing.**  Node IDs are configured at initialization.  There is no address negotiation, no DHCP equivalent, and no mechanism for nodes to join or leave without operator intervention.

**No routing.**  Every relay retransmits every packet.  There is no mechanism to direct traffic along a specific path.  In topologies where flooding is wasteful (e.g., a long chain of relays with no branching), Hoplite will consume more airtime than a routed protocol would.

**Header exposure.**  The mesh header is plaintext.  Traffic analysis (who transmits, how often, via which relay) is visible to any receiver on the correct frequency.

**Single-key trust model.**  All nodes share one key.  A compromised node exposes the key for the entire network.  Per-node key derivation is architecturally possible but not currently implemented.

---

## Applicable Deployments

Hoplite is designed for networks of 5 to 75 nodes where the developer controls the firmware on every device and needs multi-hop relay capability without adopting a complete mesh platform.

Typical deployment characteristics:

- Low to moderate traffic (telemetry intervals of minutes, not seconds)
- LoRa P2P with SX1262 radios on ESP32 platforms
- Operator-provisioned node addresses and encryption keys
- Custom application firmware with specific sensing, actuation, or data processing requirements
- Regulatory environments requiring documented duty cycle compliance (EU868/ETSI)

Typical topologies: agricultural fields, factory floors, building infrastructure, and environmental monitoring stations — sparse networks where node count is bounded and per-event reliability matters more than throughput.

---

## Underlying Driver

Hoplite runs on the SX1262 Arduino driver suite, which provides:

- Deterministic state machine with enforced transition validation
- Hardware self-test on initialization (SPI, BUSY, DIO1, chip ID verification)
- Watchdog timer with automatic recovery after consecutive failures
- Emergency reset with configuration restore
- AES-128-CCM encryption with 4-byte MAC and replay protection
- ETSI/FCC duty cycle enforcement with per-channel tracking
- Link health monitoring with silent-node detection and RSSI trending
- Thread safety via RAII mutex guards
- Silicon errata workarounds (BW500 sensitivity, SPI timing, TCXO drift)
- Board presets for Heltec V3, ESP32-S3 + Waveshare, ESP32-WROOM + Waveshare

---

## Companion Documents

| Document | Contents |
|:---------|:---------|
| Hoplite Documentation | Full API reference, configuration guide, code examples, packet format specification, security model |
| Stress Test Report | Raw test data, gateway logs, per-node statistics, capacity analysis |
| Firmware Cookbook | Validated application-level patterns: relay failover, alarm handling, gateway message dispatch |
| SX1262 Driver User's Manual | Radio driver API, board presets, hardware setup |

---

## Getting Started

Add `hoplite.h`, `hoplite.cpp`, and `hoplite_platform_bindings.cpp` to your project alongside the SX1262 driver suite.  See the Hoplite Documentation for complete API reference, configuration guide, and working examples.
