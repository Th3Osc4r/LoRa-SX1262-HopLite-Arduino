## 📊 Performance

Measured on ESP32-S3 + Waveshare SX1262 @ SF7/125kHz unless noted.

### ⏱️ Timing

| Metric | Measured | Notes |
|--------|----------|-------|
| TX-to-RX turnaround | `___` ms | STANDBY transition + IRQ clear |
| RX-to-TX turnaround | `___` ms | Ready to transmit ACK |
| Wake from WARM sleep | `___` µs | Config retained |
| Wake from COLD sleep | `___` ms | Full re-init required |
| `sx1262_init_simple()` | `___` ms | Cold boot to TX-ready |

### 🔋 Power Consumption

| State | Current | Conditions |
|-------|---------|------------|
| SLEEP (WARM) | ~600 nA | Config retained, 340 µs wake |
| SLEEP (COLD) | ~160 nA | Lowest power, 3.5 ms wake |
| STANDBY_RC | `___` µA | Idle, ready for TX/RX |
| RX (active) | `___` mA | Listening for packets |
| TX @ +14 dBm | `___` mA | Typical transmission |
| TX @ +22 dBm | `___` mA | Maximum power |

**Duty-cycled example:** 20-byte packet every 15 min @ +14 dBm, WARM sleep between  
→ Average current: `___` µA → Estimated battery life on 2000 mAh cell: `___` months

### 📡 Range & Reliability

| Scenario | SF | Distance | Packet Success Rate |
|----------|-----|----------|---------------------|
| Line-of-sight (outdoor) | SF7 | `___` m | `___`% |
| Line-of-sight (outdoor) | SF10 | `___` m | `___`% |
| Urban / obstructed | SF7 | `___` m | `___`% |
| Urban / obstructed | SF12 | `___` m | `___`% |
| Indoor (2 walls) | SF7 | `___` m | `___`% |

*Tested with `___` packets per scenario.*

### 💾 Resource Footprint

| Resource | This Driver | RadioLib* | Notes |
|----------|-------------|-----------|-------|
| Flash | `___` KB | `___` KB | Minimal build, logging disabled |
| RAM (static) | `___` KB | `___` KB | Driver structs + buffers |
| RAM (peak) | `___` KB | `___` KB | During TX/RX operation |

*\*RadioLib v`___` with SX1262 only, same board/config. Your mileage may vary.*

### 🧑‍💻 Developer Experience

| Metric | Value |
|--------|-------|
| Lines to first TX | **~10** (see Quick Start) |
| Lines to first RX | **~10** |
| Board presets | **4** (Heltec V3, ESP32-S3, ESP32-WROOM, Custom) |
| Porting effort | Change `config.h` only |
| Self-test coverage | SPI · register R/W · IRQ · BUSY · TX/RX loopback |

### ✅ Reliability Testing

| Test | Duration | Result |
|------|----------|--------|
| Continuous ping-pong | `___` hours | `___` packets, `___` failures |
| Sleep/wake cycles | `___` cycles | `___`% success |
| Hot environment (50°C) | `___` hours | Stable / `___` |
| Cold environment (−10°C) | `___` hours | Stable / `___` |

---

### 🏆 Key Differentiators

- **No 2× Time-on-Air bug** — TX completes in theoretical ToA (not double)
- **Thread-safe** — Mutex-protected state machine for RTOS use
- **Errata handled internally** — IQ polarity, implicit header, image calibration
- **One-call init** — `sx1262_init_simple()` configures everything
- **Built-in diagnostics** — `sx1262_self_test()` catches wiring issues before you chase ghosts
- **Regulatory-ready** — Duty-cycle tracking helpers for EU868 compliance


20:07:18.618 ->   WARM wake:                avg:   2571 us  min:   2566  max:   2605  (n=100)
20:07:18.618 ->   COLD wake:                avg: 121068 us  min: 121068  max: 121071  (n=100)

The COLD wake is high because it includes sx1262_driver_deinit() + sx1262_init_simple() 
— that's the full teardown/rebuild cycle. The raw chip wake from COLD is ~3.5 ms, but 
the test measures the complete "sleep → usable radio" time which is more realistic.

 +---------------------------------------------------------------------+
 |              SX1262 DRIVER TIMING BENCHMARK RESULTS                 |
 +---------------------------------------------------------------------+
 |  Board: Heltec WiFi LoRa 32 V3                                      |
 |  Frequency: 868100000 Hz   TX Power: 14 dBm                         |
 +---------------------------------------------------------------------+
 |  sx1262_init_simple():       213 ms                                 |
 |                                                                     |
 |  Wake from WARM sleep:      2571 us  (min: 2569, max: 2614)         |
 |  Wake from COLD sleep:    121082 us  (min: 121080, max: 121086)     |
 |                                                                     |
 |  TX-to-RX turnaround:         28 ms  (overhead only)                |
 +---------------------------------------------------------------------+
 
 
22:53:40.241 -> +---------------------------------------------------------------------+
22:53:40.241 -> |          SX1262-P2P DRIVER - PING-PONG RTT RESULTS                  |
22:53:40.241 -> +---------------------------------------------------------------------+
22:53:40.241 -> |  Board: Heltec WiFi LoRa 32 V3
22:53:40.241 -> |  Frequency: 868100000 Hz   TX Power: 14 dBm
22:53:40.241 -> |  Packet size: 16 bytes
22:53:40.273 -> +---------------------------------------------------------------------+
22:53:40.273 -> |  Pings sent:         100
22:53:40.273 -> |  Pongs received:     100
22:53:40.273 -> |  Timeouts:             0
22:53:40.273 -> |  Success rate:     100.0 %
22:53:40.273 -> +---------------------------------------------------------------------+
22:53:40.273 -> |  RTT average:     184153 us  (184.2 ms)
22:53:40.273 -> |  RTT minimum:     184140 us  (184.1 ms)
22:53:40.273 -> |  RTT maximum:     185003 us  (185.0 ms)
22:53:40.318 -> +---------------------------------------------------------------------+

 
23:03:16.676 -> +---------------------------------------------------------------------+
23:03:16.676 -> |              RADIOLIB - PING-PONG RTT RESULTS                       |
23:03:16.676 -> +---------------------------------------------------------------------+
23:03:16.676 -> |  Board: Heltec WiFi LoRa 32 V3
23:03:16.676 -> |  Frequency: 868.1 MHz   TX Power: 14 dBm
23:03:16.676 -> |  Packet size: 16 bytes   SF7 BW125
23:03:16.676 -> +---------------------------------------------------------------------+
23:03:16.708 -> |  Pings sent:         100
23:03:16.708 -> |  Pongs received:     100
23:03:16.708 -> |  Timeouts:             0
23:03:16.708 -> |  Success rate:     100.0 %
23:03:16.708 -> +---------------------------------------------------------------------+
23:03:16.708 -> |  RTT average:     118474 us  (118.5 ms)
23:03:16.708 -> |  RTT minimum:     118403 us  (118.4 ms)
23:03:16.708 -> |  RTT maximum:     118575 us  (118.6 ms)
23:03:16.708 -> +---------------------------------------------------------------------+