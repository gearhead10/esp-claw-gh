/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HTTP_SYSTEM_MAX_TASKS  48

static const char *TAG = "http_system";

static const char *task_state_to_string(eTaskState state)
{
    switch (state) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        case eInvalid:
        default:         return "invalid";
    }
}

static const char *task_reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

#if (configGENERATE_RUN_TIME_STATS == 1) && (configUSE_TRACE_FACILITY == 1)
static cJSON *build_tasks_array(uint32_t *out_total_runtime)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0 || task_count > HTTP_SYSTEM_MAX_TASKS) {
        task_count = (task_count > HTTP_SYSTEM_MAX_TASKS) ? HTTP_SYSTEM_MAX_TASKS : task_count;
    }

    TaskStatus_t *snapshot = calloc(task_count, sizeof(*snapshot));
    if (!snapshot) {
        return NULL;
    }

    uint32_t total_runtime = 0;
    UBaseType_t filled = uxTaskGetSystemState(snapshot, task_count, &total_runtime);
    if (out_total_runtime) {
        *out_total_runtime = total_runtime;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        free(snapshot);
        return NULL;
    }

    for (UBaseType_t i = 0; i < filled; i++) {
        const TaskStatus_t *t = &snapshot[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "id", (double)t->xTaskNumber);
        cJSON_AddStringToObject(item, "name", t->pcTaskName ? t->pcTaskName : "?");
        cJSON_AddStringToObject(item, "state", task_state_to_string(t->eCurrentState));
        cJSON_AddNumberToObject(item, "priority", (double)t->uxCurrentPriority);
        cJSON_AddNumberToObject(item, "base_priority", (double)t->uxBasePriority);
        /* Stack high water mark is reported in stack words (4 bytes on Xtensa);
         * convert to bytes so the UI can show a familiar unit. */
        cJSON_AddNumberToObject(item, "stack_free_bytes",
                                (double)t->usStackHighWaterMark * sizeof(StackType_t));
        cJSON_AddNumberToObject(item, "runtime_counter", (double)t->ulRunTimeCounter);
#if (configTASKLIST_INCLUDE_COREID == 1) || (CONFIG_FREERTOS_SMP == 1)
        /* portNO_AFFINITY is the tskNO_AFFINITY constant. */
        cJSON_AddNumberToObject(item, "core_id",
                                t->xCoreID == tskNO_AFFINITY ? -1 : (double)t->xCoreID);
#endif
        cJSON_AddItemToArray(arr, item);
    }

    free(snapshot);
    return arr;
}
#endif

static esp_err_t system_tasks_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000LL));
    cJSON_AddNumberToObject(root, "free_heap_bytes", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap_bytes", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "free_internal_bytes",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "total_internal_bytes",
                            (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram_bytes",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "total_psram_bytes",
                            (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "task_count", (double)uxTaskGetNumberOfTasks());
    cJSON_AddStringToObject(root, "reset_reason",
                            task_reset_reason_to_string(esp_reset_reason()));

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    cJSON_AddNumberToObject(root, "cpu_cores", chip.cores);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);

#if (configGENERATE_RUN_TIME_STATS == 1) && (configUSE_TRACE_FACILITY == 1)
    uint32_t total_runtime = 0;
    cJSON *tasks = build_tasks_array(&total_runtime);
    if (tasks) {
        cJSON_AddItemToObject(root, "tasks", tasks);
    }
    cJSON_AddNumberToObject(root, "total_runtime_counter", (double)total_runtime);
    cJSON_AddBoolToObject(root, "runtime_stats_available", true);
#else
    cJSON_AddItemToObject(root, "tasks", cJSON_CreateArray());
    cJSON_AddBoolToObject(root, "runtime_stats_available", false);
#endif

    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_system_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/system/tasks", .method = HTTP_GET, .handler = system_tasks_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", handlers[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
