/**
 * @file hoplite_platform_bindings.cpp
 * @brief Platform bindings: Hoplite mesh ↔ Arduino SX1262 driver
 *
 * This file implements the 10 platform-specific functions required by the
 * Hoplite core.  It is the ONLY file that touches the SX1262 Arduino driver
 * API.  To port Hoplite to a different radio or platform, rewrite this file.
 *
 * NOTE: This file is compile-time included by hoplite.cpp and should
 * NOT be compiled separately.
 *
 * License: MIT
 */

#include <Arduino.h>

#include "sx1262_driver.h"
#include "sx1262_duty_cycle.h"
#include "sx1262_security.h"
#include "sx1262_link_health.h"

#include <esp_random.h>

/* ============================================================
 * Cached LoRa Config (for airtime calculation)
 *
 * sx1262_get_time_on_air_ms() requires the current LoRa config.
 * We cache it on first call — the radio config does not change
 * during normal operation once the driver is initialized.
 * ============================================================ */

static sx1262_lora_config_t cached_lora_config;
static bool lora_config_cached = false;

static void ensure_lora_config_cached(void) {
  if (!lora_config_cached) {
    if (sx1262_get_config(&cached_lora_config) == SX1262_OK) {
      lora_config_cached = true;
    }
  }
}

/* ============================================================
 * Binding 1: Time
 * ============================================================ */

static uint32_t hoplite_get_time_ms(void) {
  return millis();
}

/* ============================================================
 * Binding 2: Delay
 * ============================================================ */

static void hoplite_delay_ms(uint32_t ms) {
  delay(ms);
}

/* ============================================================
 * Binding 3: Random Number Generator
 * ============================================================ */

static uint16_t hoplite_random_range(uint16_t min_val, uint16_t max_val) {
  if (min_val >= max_val) {
    return min_val;
  }
  uint16_t range = max_val - min_val + 1;
  uint32_t rand_val = esp_random();  /* ESP32 hardware TRNG */
  return min_val + (uint16_t)(rand_val % range);
}

/* ============================================================
 * Binding 4: Transmit
 * ============================================================ */

static hoplite_result_t hoplite_driver_tx(const uint8_t *buf, uint8_t len) {
  sx1262_tx_result_t tx_result;

  sx1262_result_t result = sx1262_transmit(buf, len, 0, &tx_result);

  switch (result) {
    case SX1262_OK:
      return HOPLITE_OK;
    case SX1262_ERROR_BUSY_TIMEOUT:
      return HOPLITE_ERR_TX_DENIED;
    case SX1262_ERROR_NOT_INITIALIZED:
      return HOPLITE_ERR_NOT_INITIALIZED;
    default:
      return HOPLITE_ERR_INTERNAL;
  }
}

/* ============================================================
 * Binding 5: Airtime Calculation
 * ============================================================ */

static uint32_t hoplite_calculate_airtime(uint8_t packet_len) {
  ensure_lora_config_cached();

  if (lora_config_cached) {
    return sx1262_get_time_on_air_ms(&cached_lora_config, packet_len);
  }

  /*
   * Fallback: conservative 500 ms estimate if config unavailable.
   * This should never happen in normal operation — the driver must
   * be initialized before Hoplite.
   */
  return 500;
}

/* ============================================================
 * Binding 6: Duty Cycle Check
 * ============================================================ */

static bool hoplite_dc_can_transmit(uint32_t airtime_ms, uint32_t *wait_ms) {
  return sx1262_dc_can_transmit(airtime_ms, wait_ms);
}

/* ============================================================
 * Binding 7: Duty Cycle Recording
 * ============================================================ */

static void hoplite_dc_record_tx(uint32_t airtime_ms) {
  sx1262_dc_record_transmission(airtime_ms);
}

/* ============================================================
 * Binding 8: Encryption
 * ============================================================ */

static hoplite_security_result_t hoplite_secure_pack(void *ctx,
                                                     uint8_t *payload,
                                                     uint8_t len,
                                                     uint8_t *out_len) {
  if (ctx == NULL) {
    *out_len = len;
    return HOPLITE_SECURITY_OK;
  }

  sx1262_security_context_t *sec_ctx = (sx1262_security_context_t *)ctx;

  sx1262_security_result_t result =
      sx1262_secure_pack(sec_ctx, payload, len, out_len);

  return (result == SX1262_SEC_OK) ? HOPLITE_SECURITY_OK : -1;
}

/* ============================================================
 * Binding 9: Decryption
 * ============================================================ */

static hoplite_security_result_t hoplite_secure_unpack(void *ctx,
                                                       uint8_t *payload,
                                                       uint8_t len,
                                                       uint8_t *out_len) {
  if (ctx == NULL) {
    *out_len = len;
    return HOPLITE_SECURITY_OK;
  }

  sx1262_security_context_t *sec_ctx = (sx1262_security_context_t *)ctx;

  sx1262_security_result_t result =
      sx1262_secure_unpack(sec_ctx, payload, len, out_len);

  return (result == SX1262_SEC_OK) ? HOPLITE_SECURITY_OK : -1;
}

/* ============================================================
 * Binding 10: Link Health (Conditional)
 * ============================================================ */

#if HOPLITE_ENABLE_LINK_HEALTH
static void hoplite_record_link_health(uint8_t node_id, int16_t rssi,
                                       int8_t snr) {
  sx1262_link_health_record_rx_auto(node_id, rssi, snr,
                                    hoplite_cfg.link_health_interval_ms);
}
#endif
