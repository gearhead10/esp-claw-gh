/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time NimBLE stack init + host task spawn. Idempotent; safe to call
 * multiple times. Must be called once during app start before
 * cap_ble_scan_register_group(). Returns ESP_OK once the BLE host has
 * synced; subsequent calls return ESP_OK immediately. */
esp_err_t cap_ble_scan_start(void);

/* Register the BLE scan capability group with claw_cap so the LLM can
 * call it. Idempotent. */
esp_err_t cap_ble_scan_register_group(void);

#ifdef __cplusplus
}
#endif
