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

/* Start the log-capture hook so diagnose_logs_recent has data to return.
 * Idempotent. Must be called once during app start before any subsystem the
 * caller wants to capture logs from. */
esp_err_t cap_diagnose_start(void);

/* Register the diagnose capability group with claw_cap so the LLM can call it. */
esp_err_t cap_diagnose_register_group(void);

#ifdef __cplusplus
}
#endif
