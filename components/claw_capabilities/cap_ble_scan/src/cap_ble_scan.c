/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_ble_scan.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "cap_ble_scan";

#ifndef CONFIG_CLAW_CAP_BLE_SCAN_MAX_RESULTS
#define CONFIG_CLAW_CAP_BLE_SCAN_MAX_RESULTS 32
#endif
#ifndef CONFIG_CLAW_CAP_BLE_SCAN_DEFAULT_DURATION_MS
#define CONFIG_CLAW_CAP_BLE_SCAN_DEFAULT_DURATION_MS 5000
#endif
#ifdef CONFIG_CLAW_CAP_BLE_SCAN_PASSIVE
#define CAP_BLE_SCAN_PASSIVE_DEFAULT 1
#else
#define CAP_BLE_SCAN_PASSIVE_DEFAULT 0
#endif

#define CAP_BLE_SCAN_DURATION_MIN_MS     500
#define CAP_BLE_SCAN_DURATION_MAX_MS     60000
#define CAP_BLE_SCAN_NAME_MAX            32
#define CAP_BLE_SCAN_MFG_DATA_MAX        32
#define CAP_BLE_SCAN_UUIDS16_MAX         8
#define CAP_BLE_SCAN_RESULTS_MAX         CONFIG_CLAW_CAP_BLE_SCAN_MAX_RESULTS
#define CAP_BLE_SCAN_DONE_TIMEOUT_PAD_MS 2000

typedef struct {
    uint8_t  addr[6];                   /* big-endian for printing */
    uint8_t  addr_type;
    int8_t   rssi_best;
    int8_t   tx_power;
    bool     has_tx_power;
    bool     name_complete;
    char     name[CAP_BLE_SCAN_NAME_MAX + 1];
    uint8_t  mfg_data[CAP_BLE_SCAN_MFG_DATA_MAX];
    uint8_t  mfg_data_len;
    uint16_t uuids16[CAP_BLE_SCAN_UUIDS16_MAX];
    uint8_t  uuids16_count;
} cap_ble_scan_device_t;

typedef struct {
    SemaphoreHandle_t scan_mutex;        /* one scanner at a time */
    SemaphoreHandle_t scan_done_sem;     /* given on BLE_GAP_EVENT_DISC_COMPLETE */
    SemaphoreHandle_t results_mutex;     /* protects results / count / truncated */
    SemaphoreHandle_t sync_sem;          /* given once when ble_hs_cfg.sync_cb fires */
    cap_ble_scan_device_t results[CAP_BLE_SCAN_RESULTS_MAX];
    size_t                results_count;
    size_t                results_cap;   /* clamped by caller-provided max_results */
    bool                  truncated;
    bool                  initialised;
    bool                  host_synced;
    uint8_t               own_addr_type;
} cap_ble_scan_runtime_t;

static cap_ble_scan_runtime_t s_ble = {0};

/* ────────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                     */
/* ────────────────────────────────────────────────────────────────────────── */

static const char *cap_ble_scan_addr_type_name(uint8_t t)
{
    switch (t) {
    case BLE_ADDR_PUBLIC:       return "public";
    case BLE_ADDR_RANDOM:       return "random";
    case BLE_ADDR_PUBLIC_ID:    return "public_id";
    case BLE_ADDR_RANDOM_ID:    return "random_id";
    default:                    return "unknown";
    }
}

static void cap_ble_scan_format_addr(const uint8_t addr_le[6], char out[18])
{
    /* NimBLE stores addresses little-endian on the wire; print in the
     * canonical big-endian colon notation expected by everyone. */
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr_le[5], addr_le[4], addr_le[3], addr_le[2], addr_le[1], addr_le[0]);
}

static void cap_ble_scan_hex_encode(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t out = 0;
    for (size_t i = 0; i < len && out + 2 < dst_size; ++i) {
        dst[out++] = hex[(src[i] >> 4) & 0xF];
        dst[out++] = hex[src[i]        & 0xF];
    }
    dst[out < dst_size ? out : dst_size - 1] = '\0';
}

static void cap_ble_scan_copy_name(const uint8_t *src, size_t src_len, char *dst,
                                   size_t dst_size)
{
    size_t copy = src_len < dst_size - 1 ? src_len : dst_size - 1;
    /* Drop control bytes so the JSON output stays printable; keep UTF-8. */
    size_t out = 0;
    for (size_t i = 0; i < copy && out < dst_size - 1; ++i) {
        uint8_t b = src[i];
        if (b == '\0') {
            break;
        }
        if (b < 0x20 && b != '\t') {
            continue;
        }
        dst[out++] = (char)b;
    }
    dst[out] = '\0';
}

static cap_ble_scan_device_t *cap_ble_scan_find_or_alloc(const ble_addr_t *addr)
{
    for (size_t i = 0; i < s_ble.results_count; ++i) {
        if (memcmp(s_ble.results[i].addr, addr->val, 6) == 0 &&
            s_ble.results[i].addr_type == addr->type) {
            return &s_ble.results[i];
        }
    }
    size_t cap = s_ble.results_cap;
    if (cap > CAP_BLE_SCAN_RESULTS_MAX) {
        cap = CAP_BLE_SCAN_RESULTS_MAX;
    }
    if (s_ble.results_count >= cap) {
        s_ble.truncated = true;
        return NULL;
    }
    cap_ble_scan_device_t *entry = &s_ble.results[s_ble.results_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->addr, addr->val, 6);
    entry->addr_type = addr->type;
    entry->rssi_best = INT8_MIN;
    entry->tx_power = 0;
    return entry;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* GAP discovery callback                                                      */
/* ────────────────────────────────────────────────────────────────────────── */

static int cap_ble_scan_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields = {0};
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        /* Even if parsing fails partially (rc != 0), we still want to record
         * the device — RSSI and address are valid independently of the adv
         * payload structure. */

        if (xSemaphoreTake(s_ble.results_mutex, 0) != pdTRUE) {
            /* Should not happen — the cap holds it across the scan; just skip. */
            return 0;
        }
        cap_ble_scan_device_t *dev = cap_ble_scan_find_or_alloc(&event->disc.addr);
        if (!dev) {
            xSemaphoreGive(s_ble.results_mutex);
            return 0;
        }

        if (event->disc.rssi > dev->rssi_best) {
            dev->rssi_best = event->disc.rssi;
        }

        if (rc == 0) {
            if (fields.name && fields.name_len > 0) {
                /* Prefer the complete name when both forms are seen across packets. */
                if (!dev->name_complete || fields.name_is_complete) {
                    cap_ble_scan_copy_name(fields.name, fields.name_len, dev->name,
                                           sizeof(dev->name));
                    dev->name_complete = fields.name_is_complete;
                }
            }
            if (fields.tx_pwr_lvl_is_present) {
                dev->tx_power = fields.tx_pwr_lvl;
                dev->has_tx_power = true;
            }
            if (fields.mfg_data && fields.mfg_data_len > 0 && dev->mfg_data_len == 0) {
                size_t copy = fields.mfg_data_len < CAP_BLE_SCAN_MFG_DATA_MAX
                              ? fields.mfg_data_len : CAP_BLE_SCAN_MFG_DATA_MAX;
                memcpy(dev->mfg_data, fields.mfg_data, copy);
                dev->mfg_data_len = (uint8_t)copy;
            }
            if (fields.uuids16 && fields.num_uuids16 > 0 && dev->uuids16_count == 0) {
                size_t copy = fields.num_uuids16 < CAP_BLE_SCAN_UUIDS16_MAX
                              ? fields.num_uuids16 : CAP_BLE_SCAN_UUIDS16_MAX;
                for (size_t i = 0; i < copy; ++i) {
                    dev->uuids16[i] = fields.uuids16[i].value;
                }
                dev->uuids16_count = (uint8_t)copy;
            }
        }

        xSemaphoreGive(s_ble.results_mutex);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGD(TAG, "scan complete reason=%d", event->disc_complete.reason);
        xSemaphoreGive(s_ble.scan_done_sem);
        return 0;
    default:
        return 0;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* NimBLE host lifecycle                                                       */
/* ────────────────────────────────────────────────────────────────────────── */

static void cap_ble_scan_on_sync(void)
{
    /* Sync fires when the controller is ready and ble_hs is operational. */
    int rc = ble_hs_id_infer_auto(0 /* privacy = 0 */, &s_ble.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        s_ble.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    s_ble.host_synced = true;
    xSemaphoreGive(s_ble.sync_sem);
}

static void cap_ble_scan_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
    s_ble.host_synced = false;
}

static void cap_ble_scan_host_task(void *arg)
{
    (void)arg;
    /* Runs until nimble_port_stop(). */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Scan driver                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int32_t  duration_ms;
    size_t   max_results;
    bool     active;
    bool     deduplicate;
    char     name_filter[CAP_BLE_SCAN_NAME_MAX + 1];
    bool     have_name_filter;
} cap_ble_scan_params_t;

static void cap_ble_scan_params_load_defaults(cap_ble_scan_params_t *p)
{
    p->duration_ms = CONFIG_CLAW_CAP_BLE_SCAN_DEFAULT_DURATION_MS;
    p->max_results = CAP_BLE_SCAN_RESULTS_MAX;
    p->active = !CAP_BLE_SCAN_PASSIVE_DEFAULT;
    p->deduplicate = true;
    p->name_filter[0] = '\0';
    p->have_name_filter = false;
}

static void cap_ble_scan_params_parse(const char *input_json, cap_ble_scan_params_t *p)
{
    cap_ble_scan_params_load_defaults(p);
    if (!input_json || !input_json[0]) {
        return;
    }
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        return;
    }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "duration_ms")) && cJSON_IsNumber(v)) {
        int32_t d = (int32_t)v->valuedouble;
        if (d < CAP_BLE_SCAN_DURATION_MIN_MS) {
            d = CAP_BLE_SCAN_DURATION_MIN_MS;
        } else if (d > CAP_BLE_SCAN_DURATION_MAX_MS) {
            d = CAP_BLE_SCAN_DURATION_MAX_MS;
        }
        p->duration_ms = d;
    }
    if ((v = cJSON_GetObjectItem(root, "max_results")) && cJSON_IsNumber(v)) {
        int n = (int)v->valuedouble;
        if (n < 1) {
            n = 1;
        } else if (n > CAP_BLE_SCAN_RESULTS_MAX) {
            n = CAP_BLE_SCAN_RESULTS_MAX;
        }
        p->max_results = (size_t)n;
    }
    if ((v = cJSON_GetObjectItem(root, "active")) && cJSON_IsBool(v)) {
        p->active = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "deduplicate")) && cJSON_IsBool(v)) {
        p->deduplicate = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "name_filter")) && cJSON_IsString(v) &&
        v->valuestring && v->valuestring[0]) {
        strlcpy(p->name_filter, v->valuestring, sizeof(p->name_filter));
        p->have_name_filter = true;
    }
    cJSON_Delete(root);
}

static esp_err_t cap_ble_scan_write_error(char *output, size_t output_size,
                                          const char *error, const char *hint)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "scan failed");
    if (hint) {
        cJSON_AddStringToObject(root, "hint", hint);
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_ble_scan_emit_results(const cap_ble_scan_params_t *params,
                                           int64_t elapsed_ms,
                                           char *output, size_t output_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "duration_ms", (double)elapsed_ms);
    cJSON_AddNumberToObject(root, "device_count", (double)s_ble.results_count);
    cJSON_AddBoolToObject(root, "truncated", s_ble.truncated);
    cJSON_AddBoolToObject(root, "active_scan", params->active);
    cJSON_AddItemToObject(root, "devices", arr);

    for (size_t i = 0; i < s_ble.results_count; ++i) {
        cap_ble_scan_device_t *d = &s_ble.results[i];

        if (params->have_name_filter) {
            /* Case-sensitive substring match — good enough for an LLM-driven
             * tool; if matching is unwanted the LLM just omits the filter. */
            if (!strstr(d->name, params->name_filter)) {
                continue;
            }
        }

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        char addr_str[18];
        cap_ble_scan_format_addr(d->addr, addr_str);
        cJSON_AddStringToObject(obj, "address", addr_str);
        cJSON_AddStringToObject(obj, "addr_type", cap_ble_scan_addr_type_name(d->addr_type));
        cJSON_AddNumberToObject(obj, "rssi", (double)d->rssi_best);
        if (d->name[0]) {
            cJSON_AddStringToObject(obj, "name", d->name);
            cJSON_AddBoolToObject(obj, "name_complete", d->name_complete);
        }
        if (d->has_tx_power) {
            cJSON_AddNumberToObject(obj, "tx_power", (double)d->tx_power);
        }
        if (d->uuids16_count > 0) {
            cJSON *uuids = cJSON_CreateArray();
            for (size_t j = 0; j < d->uuids16_count; ++j) {
                char buf[5];
                snprintf(buf, sizeof(buf), "%04x", d->uuids16[j]);
                cJSON_AddItemToArray(uuids, cJSON_CreateString(buf));
            }
            cJSON_AddItemToObject(obj, "service_uuids16", uuids);
        }
        if (d->mfg_data_len > 0) {
            char hex[CAP_BLE_SCAN_MFG_DATA_MAX * 2 + 1];
            cap_ble_scan_hex_encode(d->mfg_data, d->mfg_data_len, hex, sizeof(hex));
            cJSON_AddStringToObject(obj, "manufacturer_data", hex);
        }

        cJSON_AddItemToArray(arr, obj);
    }

    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_ble_scan_run_once(const cap_ble_scan_params_t *params,
                                       char *output, size_t output_size)
{
    if (!s_ble.initialised || !s_ble.host_synced) {
        return cap_ble_scan_write_error(output, output_size,
                                        "BLE host not ready",
                                        "Make sure CONFIG_APP_CLAW_CAP_BLE_SCAN is enabled "
                                        "and cap_ble_scan_start() succeeded at boot.");
    }

    /* Single-scan semantics: refuse rather than block forever if someone else
     * is already scanning (e.g. LLM races two tool calls). */
    if (xSemaphoreTake(s_ble.scan_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return cap_ble_scan_write_error(output, output_size,
                                        "scan already in progress",
                                        "Wait for the previous scan to finish before retrying.");
    }

    /* Make sure no leftover "done" signal lingers from a previous call. */
    xSemaphoreTake(s_ble.scan_done_sem, 0);

    if (xSemaphoreTake(s_ble.results_mutex, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(s_ble.scan_mutex);
        return cap_ble_scan_write_error(output, output_size,
                                        "results lock unavailable", NULL);
    }
    s_ble.results_count = 0;
    s_ble.truncated = false;
    s_ble.results_cap = params->max_results;
    xSemaphoreGive(s_ble.results_mutex);

    struct ble_gap_disc_params disc = {
        .itvl              = BLE_GAP_SCAN_FAST_INTERVAL_MIN,
        .window            = BLE_GAP_SCAN_FAST_WINDOW,
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = params->active ? 0 : 1,
        .filter_duplicates = params->deduplicate ? 1 : 0,
    };

    int64_t t0 = esp_timer_get_time();
    int rc = ble_gap_disc(s_ble.own_addr_type, params->duration_ms, &disc,
                          cap_ble_scan_gap_event, NULL);
    if (rc != 0) {
        xSemaphoreGive(s_ble.scan_mutex);
        char hint[64];
        snprintf(hint, sizeof(hint), "ble_gap_disc rc=%d", rc);
        return cap_ble_scan_write_error(output, output_size,
                                        "failed to start BLE scan", hint);
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(params->duration_ms + CAP_BLE_SCAN_DONE_TIMEOUT_PAD_MS);
    if (xSemaphoreTake(s_ble.scan_done_sem, wait_ticks) != pdTRUE) {
        /* Safety fallback: NimBLE should always fire DISC_COMPLETE, but cancel
         * just in case so the next call is not blocked by an active scan. */
        ESP_LOGW(TAG, "scan complete event timeout; cancelling");
        (void)ble_gap_disc_cancel();
        /* Drain a possible late completion so the next call starts clean. */
        xSemaphoreTake(s_ble.scan_done_sem, pdMS_TO_TICKS(200));
    }
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000LL;

    esp_err_t err;
    if (xSemaphoreTake(s_ble.results_mutex, portMAX_DELAY) != pdTRUE) {
        err = cap_ble_scan_write_error(output, output_size,
                                       "results lock unavailable", NULL);
    } else {
        err = cap_ble_scan_emit_results(params, elapsed_ms, output, output_size);
        xSemaphoreGive(s_ble.results_mutex);
    }

    xSemaphoreGive(s_ble.scan_mutex);
    return err;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* claw_cap descriptor                                                         */
/* ────────────────────────────────────────────────────────────────────────── */

static esp_err_t cap_ble_scan_execute_scan(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    (void)ctx;
    cap_ble_scan_params_t params;
    cap_ble_scan_params_parse(input_json, &params);
    return cap_ble_scan_run_once(&params, output, output_size);
}

static const claw_cap_descriptor_t s_ble_scan_descriptors[] = {
    {
        .id = "ble_scan",
        .name = "ble_scan",
        .family = "ble",
        .description =
            "Scan nearby BLE devices. Returns address, RSSI, name, advertised "
            "service UUIDs and manufacturer data when present. Blocks for "
            "duration_ms (default 5000, range 500-60000). Returns up to "
            "max_results entries; truncated=true if more were seen.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"duration_ms\":{\"type\":\"integer\",\"description\":"
                "\"How long to listen for advertisements (ms). Clamped to 500-60000.\"},"
            "\"max_results\":{\"type\":\"integer\",\"description\":"
                "\"Max distinct devices to return (default and hard cap depend on build config).\"},"
            "\"active\":{\"type\":\"boolean\",\"description\":"
                "\"If true, send SCAN_REQ to scannable advertisers to harvest extra data (uses more power and is detectable). Default false.\"},"
            "\"deduplicate\":{\"type\":\"boolean\",\"description\":"
                "\"If true (default), the controller filters duplicate packets from the same device.\"},"
            "\"name_filter\":{\"type\":\"string\",\"description\":"
                "\"Optional case-sensitive substring; only devices whose advertised name contains it are reported.\"}"
            "}}",
        .execute = cap_ble_scan_execute_scan,
    },
};

static const claw_cap_group_t s_ble_scan_group = {
    .group_id = "cap_ble_scan",
    .descriptors = s_ble_scan_descriptors,
    .descriptor_count = sizeof(s_ble_scan_descriptors) / sizeof(s_ble_scan_descriptors[0]),
};

/* ────────────────────────────────────────────────────────────────────────── */
/* Public lifecycle                                                            */
/* ────────────────────────────────────────────────────────────────────────── */

static esp_err_t cap_ble_scan_create_sync_primitives(void)
{
    if (!s_ble.scan_mutex) {
        s_ble.scan_mutex = xSemaphoreCreateMutex();
    }
    if (!s_ble.results_mutex) {
        s_ble.results_mutex = xSemaphoreCreateMutex();
    }
    if (!s_ble.scan_done_sem) {
        s_ble.scan_done_sem = xSemaphoreCreateBinary();
    }
    if (!s_ble.sync_sem) {
        s_ble.sync_sem = xSemaphoreCreateBinary();
    }
    if (!s_ble.scan_mutex || !s_ble.results_mutex || !s_ble.scan_done_sem || !s_ble.sync_sem) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cap_ble_scan_start(void)
{
    if (s_ble.initialised) {
        return ESP_OK;
    }

    esp_err_t err = cap_ble_scan_create_sync_primitives();
    if (err != ESP_OK) {
        return err;
    }

    /* BLE controller requires NVS for calibration data. nvs_flash_init() is
     * idempotent and harmless if NVS has already been initialised by the
     * caller earlier in boot. */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = cap_ble_scan_on_sync;
    ble_hs_cfg.reset_cb = cap_ble_scan_on_reset;

    nimble_port_freertos_init(cap_ble_scan_host_task);

    /* Wait up to 5 seconds for ble_hs to come up. Without this guard the first
     * cap_ble_scan_execute call would race the host and return "not ready". */
    if (xSemaphoreTake(s_ble.sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "BLE host sync timeout; cap_ble_scan will error until sync arrives");
    }

    s_ble.initialised = true;
    ESP_LOGI(TAG, "BLE scanner ready (own_addr_type=%u, host_synced=%d)",
             s_ble.own_addr_type, s_ble.host_synced ? 1 : 0);
    return ESP_OK;
}

esp_err_t cap_ble_scan_register_group(void)
{
    if (claw_cap_group_exists(s_ble_scan_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_ble_scan_group);
}
