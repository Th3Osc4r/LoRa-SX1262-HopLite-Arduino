#ifndef HOPLITE_H
#define HOPLITE_H

/* ============================================================================
 * Hoplite — Lightweight Multi-Hop Mesh Layer for LoRa P2P
 * Public API (v1.0)
 *
 * Hoplite provides managed-flooding mesh networking on top of an existing
 * SX1262 LoRa point-to-point driver.  The core logic is platform-agnostic;
 * all hardware coupling lives in hoplite_platform_bindings.cpp.
 *
 * Architecture:
 *   +------------------+
 *   | Application      |
 *   +------------------+
 *           |
 *   +------------------+
 *   | Hoplite          |  <-- This API
 *   +------------------+
 *           |
 *   +------------------+
 *   | SX1262 Driver    |
 *   +------------------+
 *           |
 *   +------------------+
 *   | SX1262 Hardware  |
 *   +------------------+
 *
 * License: MIT
 * ============================================================================
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Version
 * ============================================================================
 */

#define HOPLITE_API_VERSION_MAJOR  1
#define HOPLITE_API_VERSION_MINOR  0

/* ============================================================================
 * Compile-Time Configuration
 * ============================================================================
 */

/* Maximum payload size (excluding mesh header) */
#define HOPLITE_MAX_PAYLOAD_LEN  240u

/* Maximum TTL value (4-bit field) */
#define HOPLITE_MAX_TTL  15u

/* ----------------------------------------------------------------------------
 * Compile-Time Feature Controls (override before including this header)
 * ----------------------------------------------------------------------------
 */

/* Maximum relay deferrals before drop (duty cycle exhaustion) */
#ifndef HOPLITE_MAX_RELAY_DEFERRALS
#define HOPLITE_MAX_RELAY_DEFERRALS  3
#endif

/* Relay queue depth */
#ifndef HOPLITE_RELAY_QUEUE_SIZE
#define HOPLITE_RELAY_QUEUE_SIZE  4u
#endif

/* Deduplication cache size (entries) */
#ifndef HOPLITE_DEDUP_CACHE_SIZE
#define HOPLITE_DEDUP_CACHE_SIZE  16u
#endif

/* Statistics collection (1 = enabled, 0 = disabled) */
#ifndef HOPLITE_ENABLE_STATISTICS
#define HOPLITE_ENABLE_STATISTICS  1
#endif

/* Link health integration (1 = enabled, 0 = disabled) */
#ifndef HOPLITE_ENABLE_LINK_HEALTH
#define HOPLITE_ENABLE_LINK_HEALTH  0
#endif

/* ============================================================================
 * Special Addresses
 * ============================================================================
 */

#define HOPLITE_ADDR_BROADCAST  0x00u
#define HOPLITE_ADDR_GATEWAY    0xFFu

/* ============================================================================
 * Result Codes
 * ============================================================================
 */

typedef enum {
  HOPLITE_OK = 0,

  /* Configuration / State Errors */
  HOPLITE_ERR_NOT_INITIALIZED    = -1,
  HOPLITE_ERR_INVALID_PARAM      = -2,
  HOPLITE_ERR_INVALID_STATE      = -3,  /* Also: called from callback context */

  /* Transmission / Policy Errors */
  HOPLITE_ERR_TX_DENIED          = -10, /* Duty cycle or policy denial */
  HOPLITE_ERR_TTL_EXPIRED        = -11,
  HOPLITE_ERR_DUPLICATE_PKT      = -12,
  HOPLITE_ERR_ROLE_FORBIDDEN     = -13,
  HOPLITE_ERR_DECRYPT_FAIL       = -14, /* Decryption or authentication failure */
  HOPLITE_ERR_PROTOCOL_VIOLATION = -15, /* Malformed packet violates protocol */
  HOPLITE_ERR_ENCRYPT_FAIL       = -16, /* Encryption failure */

  /* Internal Errors */
  HOPLITE_ERR_INTERNAL           = -20

} hoplite_result_t;

/* ============================================================================
 * Mesh Roles
 * ============================================================================
 */

typedef enum {
  HOPLITE_ROLE_ENDPOINT = 0,  /* Sends and receives only */
  HOPLITE_ROLE_REPEATER,      /* May relay packets */
  HOPLITE_ROLE_GATEWAY        /* Protocol-privileged node */
} hoplite_role_t;

/* ============================================================================
 * Mesh Packet Header (Normative Layout)
 * ============================================================================
 *
 * 5-byte fixed header, packed.
 *
 * byte 0: packet_id
 * byte 1: sender_id
 * byte 2: dest_id
 * byte 3: [flags:4 | ttl:4]
 * byte 4: last_relay_id (0x00 = direct, else relay's node_id)
 */

typedef struct __attribute__((packed)) {
  uint8_t packet_id;
  uint8_t sender_id;
  uint8_t dest_id;
  uint8_t flags_ttl;
  uint8_t last_relay_id;  /* 0x00 = direct path, else relay's node_id */
} hoplite_header_t;

/* ============================================================================
 * Flag Masks (upper 4 bits of flags_ttl byte)
 * ============================================================================
 */

#define HOPLITE_FLAG_ENCRYPTED   0x10u  /* Bit 4: Payload is encrypted */
#define HOPLITE_FLAG_RESERVED1   0x20u  /* Bit 5: Reserved */
#define HOPLITE_FLAG_RESERVED2   0x40u  /* Bit 6: Reserved */
#define HOPLITE_FLAG_RESERVED3   0x80u  /* Bit 7: Reserved */

/* TTL mask (lower 4 bits) */
#define HOPLITE_TTL_MASK  0x0Fu

/* ============================================================================
 * Configuration Structure
 * ============================================================================
 */

typedef struct {
  uint8_t node_id;          /* Node address (1-254, 0=broadcast, 255=gateway) */
  hoplite_role_t role;      /* ENDPOINT, REPEATER, or GATEWAY */
  uint8_t default_ttl;      /* Initial TTL for originated packets (max 15) */

  uint16_t relay_delay_min_ms; /* Minimum random relay delay */
  uint16_t relay_delay_max_ms; /* Maximum random relay delay */

  uint8_t dedup_cache_size; /* Size of seen-packet cache (max
                               HOPLITE_DEDUP_CACHE_SIZE) */

  /**
   * Optional TX jitter for originated packets (milliseconds).
   * When > 0, hoplite_send() applies a random delay (0 to tx_jitter_max_ms)
   * before transmission to reduce collision probability in multi-node
   * deployments with synchronized transmission intervals.
   * Set to 0 to disable (immediate TX, default behavior).
   */
  uint16_t tx_jitter_max_ms;

  /**
   * Opaque security context pointer.
   * For the Arduino SX1262 driver, pass a pointer to an initialized
   * sx1262_security_context_t.  The platform bindings handle the cast.
   * Set to NULL to disable encryption (broadcast-only operation).
   */
  void *security_ctx;

  /**
   * Expected interval between packets from each node (milliseconds).
   * Used by link health monitor for timeout detection.
   * Set to 0 to use default (60000 ms).
   * Only effective if HOPLITE_ENABLE_LINK_HEALTH is defined.
   */
  uint32_t link_health_interval_ms;
} hoplite_config_t;

/* ============================================================================
 * RX Metadata
 * ============================================================================
 */

typedef struct {
  uint8_t origin_id;
  uint8_t dest_id;
  uint8_t ttl;
  uint8_t flags;
  uint8_t last_relay_id;  /* 0x00 = direct path, else relay's node_id */

  int16_t rssi_dbm;
  int8_t snr_db;

  uint32_t rx_time_ms;  /* Platform-provided RX timestamp */
} hoplite_rx_meta_t;

/* ============================================================================
 * Statistics
 * ============================================================================
 */

typedef struct {
  /* TX Path */
  uint32_t tx_originated;
  uint32_t tx_succeeded;
  uint32_t tx_duty_cycle_denied;
  uint32_t tx_encrypt_failed;

  /* RX Path */
  uint32_t rx_received;
  uint32_t rx_delivered;
  uint32_t rx_dropped_duplicate;
  uint32_t rx_dropped_ttl;
  uint32_t rx_dropped_protocol;
  uint32_t rx_decrypt_failed;

  /* Relay Path */
  uint32_t relay_scheduled;
  uint32_t relay_transmitted;
  uint32_t relay_deferred;
  uint32_t relay_dropped_duty;
  uint32_t relay_dropped_overflow;

  /* Callback */
  uint32_t callback_reentry_blocked;

  /* Link Health */
  uint32_t link_health_recorded;
} hoplite_statistics_t;

/* ============================================================================
 * Callback Type
 * ============================================================================
 */

typedef void (*hoplite_deliver_cb_t)(const uint8_t *payload, uint8_t length,
                                     const hoplite_rx_meta_t *meta);

/* ============================================================================
 * Public API
 * ============================================================================
 */

/**
 * Initialize Hoplite mesh state.
 *
 * Must be called after the SX1262 driver is initialized and configured.
 *
 * @param config     Ptr to configuration structure
 * @return HOPLITE_OK on success
 */
hoplite_result_t hoplite_init(const hoplite_config_t *config);

/**
 * Send a packet into the mesh.
 *
 * Blocking semantics:
 * - Non-blocking (returns immediately after TX)
 *
 * Ownership:
 * - Payload buffer is owned by the caller
 * - Buffer MUST remain valid for the duration of the call only
 *
 * Rules:
 * - Broadcast packets SHALL NOT request ACKs
 * - Encryption is mandatory for unicast when security_ctx is provided
 *
 * @param dest_id    Destination address (0 = broadcast)
 * @param payload    Pointer to payload buffer
 * @param length     Payload length (<= HOPLITE_MAX_PAYLOAD_LEN)
 * @param flags      Optional flags (ORed with protocol defaults)
 * @return HOPLITE_OK on acceptance, error otherwise
 */
hoplite_result_t hoplite_send(uint8_t dest_id, const uint8_t *payload,
                              uint8_t length, uint8_t flags);

/**
 * Process a received raw mesh packet.
 *
 * This function MUST be called exactly once per received packet.
 * NOT ISR-safe.
 *
 * @param buffer     Pointer to received packet buffer (modified in-place)
 * @param length     Total packet length (header + payload)
 * @param meta       RX metadata (RSSI, SNR, timestamp, etc.)
 * @return Result of processing (HOPLITE_OK, or drop reason)
 */
hoplite_result_t hoplite_process_rx(uint8_t *buffer, uint8_t length,
                                    const hoplite_rx_meta_t *meta);

/**
 * Register the callback invoked when a packet is delivered locally.
 *
 * @param cb         Callback function pointer
 */
void hoplite_register_deliver_callback(hoplite_deliver_cb_t cb);

/**
 * Periodic processing — advances relay scheduling and internal timers.
 *
 * Call this from loop() at least once per relay delay interval.
 *
 * @param now_ms     Current time in monotonic milliseconds (millis())
 */
void hoplite_tick(uint32_t now_ms);

/**
 * Uninitialize and reset all state.
 */
void hoplite_deinit(void);

/**
 * Get current statistics snapshot.
 *
 * @param stats      Pointer to stats structure to populate
 */
void hoplite_get_statistics(hoplite_statistics_t *stats);

/**
 * Reset statistics counters to zero.
 */
void hoplite_reset_statistics(void);

#ifdef __cplusplus
}
#endif

#endif /* HOPLITE_H */
