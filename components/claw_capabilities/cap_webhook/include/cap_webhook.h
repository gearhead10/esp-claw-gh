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

/**
 * Set the JSON-encoded webhook registry. Expected shape:
 *   [{"name":"slack","url":"https://...","method":"POST","description":"..."}]
 * Pass NULL or "" to clear the registry. Subsequent webhook_trigger calls
 * will resolve names against this list.
 */
esp_err_t cap_webhook_set_registry(const char *json);

/**
 * Register the cap_webhook capability group with claw_cap.
 */
esp_err_t cap_webhook_register_group(void);

#ifdef __cplusplus
}
#endif
