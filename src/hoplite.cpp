/*
 * Hoplite — Lightweight Multi-Hop Mesh Layer for LoRa P2P
 * Core Implementation
 * -------------------------------------------------------
 * Status: Production-Ready (Arduino)
 *
 * This file contains the core mesh logic.  It is platform-agnostic;
 * all hardware coupling lives in hoplite_platform_bindings.cpp, which
 * is compile-time included at the bottom of this file.
 *
 * License: MIT
 */

#include "hoplite.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Internal Compile-Time Parameters                                           */
/* -------------------------------------------------------------------------- */

#define HOPLITE_MAX_PACKET_LEN \
  (5 + HOPLITE_MAX_PAYLOAD_LEN + 20) /* header(5) + payload + security overhead */

/* -------------------------------------------------------------------------- */
/* Statistics Macros                                                           */
/* -------------------------------------------------------------------------- */

#if HOPLITE_ENABLE_STATISTICS
static hoplite_statistics_t hoplite_stats;
#define HOPLITE_STAT_INC(field) (hoplite_stats.field++)
#else
#define HOPLITE_STAT_INC(field) ((void)0)
#endif

#if HOPLITE_ENABLE_STATISTICS && HOPLITE_ENABLE_LINK_HEALTH
#define HOPLITE_STAT_INC_LINK_HEALTH() (hoplite_stats.link_health_recorded++)
#else
#define HOPLITE_STAT_INC_LINK_HEALTH() ((void)0)
#endif

/* -------------------------------------------------------------------------- */
/* Internal Data Structures                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
  uint8_t origin_id;
  uint8_t packet_id;
} hoplite_seen_entry_t;

typedef enum {
  RELAY_IDLE = 0,
  RELAY_PENDING,
  RELAY_TRANSMITTING
} relay_state_t;

typedef struct {
  relay_state_t state;
  uint8_t buffer[HOPLITE_MAX_PACKET_LEN];
  uint8_t length;
  uint32_t scheduled_time_ms;
  uint8_t defer_count;
} relay_entry_t;

/* -------------------------------------------------------------------------- */
/* Static State                                                                */
/* -------------------------------------------------------------------------- */

static bool hoplite_initialized = false;
static hoplite_config_t hoplite_cfg;

static hoplite_seen_entry_t seen_cache[HOPLITE_DEDUP_CACHE_SIZE];
static uint8_t seen_cache_index = 0;

static relay_entry_t relay_queue[HOPLITE_RELAY_QUEUE_SIZE];

static hoplite_deliver_cb_t deliver_cb = NULL;

static bool in_callback_context = false;

static uint8_t local_packet_id = 0;

/* -------------------------------------------------------------------------- */
/* Internal Function Prototypes (Platform Bindings)                            */
/* -------------------------------------------------------------------------- */

static uint32_t hoplite_get_time_ms(void);
static uint16_t hoplite_random_range(uint16_t min_ms, uint16_t max_ms);
static hoplite_result_t hoplite_driver_tx(const uint8_t *buf, uint8_t len);
static uint32_t hoplite_calculate_airtime(uint8_t packet_len);
static bool hoplite_dc_can_transmit(uint32_t airtime_ms, uint32_t *wait_ms);
static void hoplite_dc_record_tx(uint32_t airtime_ms);
static void hoplite_delay_ms(uint32_t ms);

typedef int hoplite_security_result_t;
#define HOPLITE_SECURITY_OK 0

static hoplite_security_result_t hoplite_secure_pack(void *ctx, uint8_t *payload,
                                                     uint8_t len, uint8_t *out_len);
static hoplite_security_result_t hoplite_secure_unpack(void *ctx, uint8_t *payload,
                                                       uint8_t len, uint8_t *out_len);

#if HOPLITE_ENABLE_LINK_HEALTH
static void hoplite_record_link_health(uint8_t node_id, int16_t rssi, int8_t snr);
#endif

/* -------------------------------------------------------------------------- */
/* Packet ID Generator                                                         */
/* -------------------------------------------------------------------------- */

static uint8_t hoplite_next_packet_id(void) {
  uint8_t pid = local_packet_id;
  local_packet_id++; /* natural wraparound */
  return pid;
}

/* -------------------------------------------------------------------------- */
/* Deduplication Logic                                                         */
/* -------------------------------------------------------------------------- */

static bool hoplite_is_duplicate(uint8_t origin_id, uint8_t packet_id) {
  for (uint8_t i = 0; i < HOPLITE_DEDUP_CACHE_SIZE; i++) {
    if (seen_cache[i].origin_id == origin_id &&
        seen_cache[i].packet_id == packet_id) {
      return true;
    }
  }
  return false;
}

static void hoplite_record_seen(uint8_t origin_id, uint8_t packet_id) {
  seen_cache[seen_cache_index].origin_id = origin_id;
  seen_cache[seen_cache_index].packet_id = packet_id;

  seen_cache_index++;
  if (seen_cache_index >= HOPLITE_DEDUP_CACHE_SIZE) {
    seen_cache_index = 0;
  }
}

/* -------------------------------------------------------------------------- */
/* Relay Queue Handling                                                        */
/* -------------------------------------------------------------------------- */

static relay_entry_t *hoplite_find_idle_relay_slot(void) {
  for (uint8_t i = 0; i < HOPLITE_RELAY_QUEUE_SIZE; i++) {
    if (relay_queue[i].state == RELAY_IDLE) {
      return &relay_queue[i];
    }
  }
  return NULL;
}

static relay_entry_t *hoplite_find_oldest_pending_slot(void) {
  relay_entry_t *oldest = NULL;
  uint32_t oldest_time = UINT32_MAX;

  for (uint8_t i = 0; i < HOPLITE_RELAY_QUEUE_SIZE; i++) {
    if (relay_queue[i].state == RELAY_PENDING) {
      if (relay_queue[i].scheduled_time_ms < oldest_time) {
        oldest_time = relay_queue[i].scheduled_time_ms;
        oldest = &relay_queue[i];
      }
    }
  }
  return oldest;
}

static void hoplite_schedule_relay(const uint8_t *packet, uint8_t length) {
  relay_entry_t *slot = hoplite_find_idle_relay_slot();

  if (!slot) {
    /* Queue full — evict oldest (FIFO) */
    slot = hoplite_find_oldest_pending_slot();

    if (!slot) {
      /* All slots TRANSMITTING — cannot evict, drop incoming */
      HOPLITE_STAT_INC(relay_dropped_overflow);
      return;
    }

    /* Evict oldest */
    HOPLITE_STAT_INC(relay_dropped_overflow);
    slot->state = RELAY_IDLE;
  }

  memcpy(slot->buffer, packet, length);
  slot->buffer[4] = hoplite_cfg.node_id;  /* Stamp relay ID */
  slot->length = length;
  slot->defer_count = 0;

  uint32_t now = hoplite_get_time_ms();
  uint16_t jitter = hoplite_random_range(hoplite_cfg.relay_delay_min_ms,
                                         hoplite_cfg.relay_delay_max_ms);

  slot->scheduled_time_ms = now + jitter;
  slot->state = RELAY_PENDING;

  HOPLITE_STAT_INC(relay_scheduled);
}

/* -------------------------------------------------------------------------- */
/* Public API Implementation                                                   */
/* -------------------------------------------------------------------------- */

hoplite_result_t hoplite_init(const hoplite_config_t *config) {
  if (!config) {
    return HOPLITE_ERR_INVALID_PARAM;
  }
  if (hoplite_initialized) {
    return HOPLITE_ERR_INVALID_STATE;
  }

  /* Validate configuration parameters */
  if (config->default_ttl > HOPLITE_MAX_TTL) {
    return HOPLITE_ERR_INVALID_PARAM;
  }
  if (config->relay_delay_min_ms > config->relay_delay_max_ms) {
    return HOPLITE_ERR_INVALID_PARAM;
  }
  if (config->dedup_cache_size == 0 ||
      config->dedup_cache_size > HOPLITE_DEDUP_CACHE_SIZE) {
    return HOPLITE_ERR_INVALID_PARAM;
  }

  /* Node ID 0x00 is broadcast address — cannot be claimed */
  if (config->node_id == HOPLITE_ADDR_BROADCAST) {
    return HOPLITE_ERR_INVALID_PARAM;
  }

  /* Only GATEWAY role may claim reserved gateway address 0xFF */
  if (config->node_id == HOPLITE_ADDR_GATEWAY &&
      config->role != HOPLITE_ROLE_GATEWAY) {
    return HOPLITE_ERR_INVALID_PARAM;
  }

  hoplite_cfg = *config;
  memset(seen_cache, 0, sizeof(seen_cache));
  memset(relay_queue, 0, sizeof(relay_queue));

  seen_cache_index = 0;
  in_callback_context = false;
  local_packet_id = 0;
  hoplite_initialized = true;

#if HOPLITE_ENABLE_STATISTICS
  memset(&hoplite_stats, 0, sizeof(hoplite_statistics_t));
#endif

  return HOPLITE_OK;
}

hoplite_result_t hoplite_send(uint8_t dest_id, const uint8_t *payload,
                              uint8_t length, uint8_t flags) {
  if (!hoplite_initialized) {
    return HOPLITE_ERR_NOT_INITIALIZED;
  }

  if (in_callback_context) {
    HOPLITE_STAT_INC(callback_reentry_blocked);
    return HOPLITE_ERR_INVALID_STATE;
  }

  if (!payload || length == 0 || length > HOPLITE_MAX_PAYLOAD_LEN) {
    return HOPLITE_ERR_INVALID_PARAM;
  }

  /* Gateway privilege: only GATEWAY may originate broadcasts */
  if (dest_id == HOPLITE_ADDR_BROADCAST &&
      hoplite_cfg.role != HOPLITE_ROLE_GATEWAY) {
    return HOPLITE_ERR_ROLE_FORBIDDEN;
  }

  HOPLITE_STAT_INC(tx_originated);

  uint8_t packet[HOPLITE_MAX_PACKET_LEN];
  uint8_t payload_len = length;
  uint8_t total_len;

  /* Copy payload to packet buffer (after header space) */
  memcpy(&packet[5], payload, length);

  /* Encryption (unicast only) */
  if (dest_id != 0 && hoplite_cfg.security_ctx != NULL) {
    uint8_t encrypted_len;
    hoplite_security_result_t sec_result = hoplite_secure_pack(
        hoplite_cfg.security_ctx, &packet[5], payload_len, &encrypted_len);

    if (sec_result != HOPLITE_SECURITY_OK) {
      HOPLITE_STAT_INC(tx_encrypt_failed);
      return HOPLITE_ERR_ENCRYPT_FAIL;
    }

    payload_len = encrypted_len;
    flags |= HOPLITE_FLAG_ENCRYPTED;
  }

  total_len = 5 + payload_len;

  /* Header packing */
  packet[0] = hoplite_next_packet_id();
  packet[1] = hoplite_cfg.node_id;
  packet[2] = dest_id;
  packet[3] = flags | (hoplite_cfg.default_ttl & 0x0F);
  packet[4] = 0x00;  /* Direct from originator, not yet relayed */

  /* Duty Cycle Check */
  uint32_t airtime_ms = hoplite_calculate_airtime(total_len);
  uint32_t wait_ms;
  if (!hoplite_dc_can_transmit(airtime_ms, &wait_ms)) {
    HOPLITE_STAT_INC(tx_duty_cycle_denied);
    return HOPLITE_ERR_TX_DENIED;
  }

  /* TX Jitter (optional collision avoidance) */
  if (hoplite_cfg.tx_jitter_max_ms > 0) {
    uint16_t jitter_delay = hoplite_random_range(0, hoplite_cfg.tx_jitter_max_ms);
    if (jitter_delay > 0) {
      hoplite_delay_ms(jitter_delay);
    }
  }

  /* Transmit */
  hoplite_result_t result = hoplite_driver_tx(packet, total_len);

  if (result == HOPLITE_OK) {
    hoplite_dc_record_tx(airtime_ms);
    HOPLITE_STAT_INC(tx_succeeded);
  }

  return result;
}

hoplite_result_t hoplite_process_rx(uint8_t *buffer, uint8_t length,
                                    const hoplite_rx_meta_t *meta) {
  if (!hoplite_initialized || !buffer || length < 5) {
    return HOPLITE_ERR_INVALID_PARAM;
  }

  if (in_callback_context) {
    HOPLITE_STAT_INC(callback_reentry_blocked);
    return HOPLITE_ERR_INVALID_STATE;
  }

  HOPLITE_STAT_INC(rx_received);

  /* Parse header */
  uint8_t packet_id    = buffer[0];
  uint8_t origin_id    = buffer[1];
  uint8_t dest_id      = buffer[2];
  uint8_t flags_ttl    = buffer[3];
  uint8_t last_relay_id = buffer[4];
  uint8_t ttl          = flags_ttl & 0x0F;
  uint8_t flags        = flags_ttl & 0xF0;

  /* Protocol violation: encrypted broadcast */
  if (dest_id == 0 && (flags & HOPLITE_FLAG_ENCRYPTED)) {
    HOPLITE_STAT_INC(rx_dropped_protocol);
    return HOPLITE_ERR_PROTOCOL_VIOLATION;
  }

  /* TTL check */
  if (ttl == 0) {
    HOPLITE_STAT_INC(rx_dropped_ttl);
    return HOPLITE_ERR_TTL_EXPIRED;
  }

  /* Deduplication */
  if (hoplite_is_duplicate(origin_id, packet_id)) {
    HOPLITE_STAT_INC(rx_dropped_duplicate);
    return HOPLITE_ERR_DUPLICATE_PKT;
  }

  hoplite_record_seen(origin_id, packet_id);

  /* Link health recording */
#if HOPLITE_ENABLE_LINK_HEALTH
  if (meta != NULL) {
    hoplite_record_link_health(origin_id, meta->rssi_dbm, meta->snr_db);
    HOPLITE_STAT_INC_LINK_HEALTH();
  }
#endif

  /* Relay decision (before delivery, operates on encrypted payload) */
  if (dest_id != hoplite_cfg.node_id &&
      hoplite_cfg.role != HOPLITE_ROLE_ENDPOINT) {
    /* Decrement TTL */
    buffer[3] = (flags_ttl & 0xF0) | (ttl - 1);
    hoplite_schedule_relay(buffer, length);
  }

  /* Local delivery */
  if (dest_id == hoplite_cfg.node_id || dest_id == 0) {
    uint8_t *payload_ptr = &buffer[5];
    uint8_t payload_len  = length - 5;

    /* Decrypt if needed (unicast only) */
    if ((flags & HOPLITE_FLAG_ENCRYPTED) && hoplite_cfg.security_ctx != NULL) {
      uint8_t decrypted_len;
      hoplite_security_result_t sec_result = hoplite_secure_unpack(
          hoplite_cfg.security_ctx, payload_ptr, payload_len, &decrypted_len);

      if (sec_result != HOPLITE_SECURITY_OK) {
        HOPLITE_STAT_INC(rx_decrypt_failed);
        return HOPLITE_ERR_DECRYPT_FAIL;
      }

      payload_len = decrypted_len;
    }

    /* Deliver to application */
    if (deliver_cb) {
      hoplite_rx_meta_t delivery_meta;
      delivery_meta.origin_id    = origin_id;
      delivery_meta.dest_id      = dest_id;
      delivery_meta.ttl          = ttl;
      delivery_meta.flags        = flags;
      delivery_meta.last_relay_id = last_relay_id;
      delivery_meta.rssi_dbm     = meta ? meta->rssi_dbm : 0;
      delivery_meta.snr_db       = meta ? meta->snr_db   : 0;
      delivery_meta.rx_time_ms   = meta ? meta->rx_time_ms : 0;

      in_callback_context = true;
      deliver_cb(payload_ptr, payload_len, &delivery_meta);
      in_callback_context = false;

      HOPLITE_STAT_INC(rx_delivered);
    }
  }

  return HOPLITE_OK;
}

void hoplite_register_deliver_callback(hoplite_deliver_cb_t cb) {
  deliver_cb = cb;
}

void hoplite_tick(uint32_t now_ms) {
  if (in_callback_context) {
    return;
  }

  for (uint8_t i = 0; i < HOPLITE_RELAY_QUEUE_SIZE; i++) {
    relay_entry_t *e = &relay_queue[i];

    if (e->state == RELAY_PENDING && now_ms >= e->scheduled_time_ms) {

      /* Duty Cycle Check */
      uint32_t airtime_ms = hoplite_calculate_airtime(e->length);
      uint32_t wait_ms;

      if (!hoplite_dc_can_transmit(airtime_ms, &wait_ms)) {
        /* Duty cycle denied — defer or drop */
        e->defer_count++;
        HOPLITE_STAT_INC(relay_deferred);

        if (e->defer_count > HOPLITE_MAX_RELAY_DEFERRALS) {
          /* Exceeded max deferrals — drop */
          HOPLITE_STAT_INC(relay_dropped_duty);
          e->state = RELAY_IDLE;
        } else {
          /* Defer: reschedule for later */
          e->scheduled_time_ms = now_ms + wait_ms;
        }
        continue;
      }

      /* Duty cycle permits — transmit */
      e->state = RELAY_TRANSMITTING;
      hoplite_result_t tx_result = hoplite_driver_tx(e->buffer, e->length);

      if (tx_result == HOPLITE_OK) {
        hoplite_dc_record_tx(airtime_ms);
        HOPLITE_STAT_INC(relay_transmitted);
      }

      e->state = RELAY_IDLE;
    }
  }
}

void hoplite_deinit(void) {
  if (in_callback_context) {
    return;
  }

  hoplite_initialized = false;
  deliver_cb = NULL;
  in_callback_context = false;
}

void hoplite_get_statistics(hoplite_statistics_t *stats) {
  if (!stats) {
    return;
  }

#if HOPLITE_ENABLE_STATISTICS
  *stats = hoplite_stats;
#else
  memset(stats, 0, sizeof(hoplite_statistics_t));
#endif
}

void hoplite_reset_statistics(void) {
#if HOPLITE_ENABLE_STATISTICS
  memset(&hoplite_stats, 0, sizeof(hoplite_statistics_t));
#endif
}

/* -------------------------------------------------------------------------- */
/* Platform Binding Inclusion                                                   */
/* -------------------------------------------------------------------------- */

/*
 * Platform bindings are implemented in hoplite_platform_bindings.cpp.
 * They are compile-time included here so they can be static and inlineable,
 * while keeping logical separation of platform-specific code.
 */
#include "hoplite_platform_bindings.cpp"
