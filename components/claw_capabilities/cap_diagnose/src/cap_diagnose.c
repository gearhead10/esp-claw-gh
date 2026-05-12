/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_diagnose.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "cap_diagnose";

#ifndef CONFIG_CLAW_CAP_DIAGNOSE_RING_CAPACITY
#define CONFIG_CLAW_CAP_DIAGNOSE_RING_CAPACITY 64
#endif
#ifndef CONFIG_CLAW_CAP_DIAGNOSE_MAX_LINE_LEN
#define CONFIG_CLAW_CAP_DIAGNOSE_MAX_LINE_LEN  240
#endif

#define CAP_DIAGNOSE_TAG_LEN  24

typedef struct {
    int64_t timestamp_us;
    char    level;                                          /* 'I','W','E','D','V','?' */
    char    tag[CAP_DIAGNOSE_TAG_LEN];
    char    message[CONFIG_CLAW_CAP_DIAGNOSE_MAX_LINE_LEN];
} cap_diagnose_log_entry_t;

typedef struct {
    cap_diagnose_log_entry_t *entries;
    size_t                    capacity;
    size_t                    write_pos;
    size_t                    count;
    SemaphoreHandle_t         mutex;
    vprintf_like_t            prev_vprintf;
    bool                      installed;
} cap_diagnose_runtime_t;

static cap_diagnose_runtime_t s_diag = {0};

/* Recursion guard: while we are inside the hook we must not feed lines back into
 * the ring (which would happen if any helper here uses ESP_LOGx). */
static __thread bool s_diag_in_hook = false;

static bool cap_diagnose_lock(void)
{
    if (!s_diag.mutex) {
        return false;
    }
    return xSemaphoreTake(s_diag.mutex, portMAX_DELAY) == pdTRUE;
}

static void cap_diagnose_unlock(void)
{
    if (s_diag.mutex) {
        xSemaphoreGive(s_diag.mutex);
    }
}

static char cap_diagnose_level_from_prefix(const char *line)
{
    /* IDF log lines start with "<level> (<ts>) <tag>: ...". */
    if (!line || !line[0]) {
        return '?';
    }
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' ||
         line[0] == 'D' || line[0] == 'V') && line[1] == ' ') {
        return line[0];
    }
    /* Some log macros strip the colour escape; tolerate ANSI prefix. */
    const char *cursor = line;
    if (cursor[0] == 0x1B && cursor[1] == '[') {
        const char *m = strchr(cursor, 'm');
        if (m) {
            cursor = m + 1;
        }
    }
    if ((cursor[0] == 'I' || cursor[0] == 'W' || cursor[0] == 'E' ||
         cursor[0] == 'D' || cursor[0] == 'V') && cursor[1] == ' ') {
        return cursor[0];
    }
    return '?';
}

static const char *cap_diagnose_strip_ansi(const char *line)
{
    if (line[0] == 0x1B && line[1] == '[') {
        const char *m = strchr(line, 'm');
        if (m) {
            return m + 1;
        }
    }
    return line;
}

static void cap_diagnose_parse_tag(const char *line, char *out_tag, size_t out_size)
{
    if (!out_tag || out_size == 0) {
        return;
    }
    out_tag[0] = '\0';

    const char *cursor = cap_diagnose_strip_ansi(line);
    /* Skip "I " etc. */
    if (cursor[0] && cursor[1] == ' ') {
        cursor += 2;
    }
    /* Skip "(<ts>) " */
    if (cursor[0] == '(') {
        const char *close = strchr(cursor, ')');
        if (close && close[1] == ' ') {
            cursor = close + 2;
        }
    }
    /* Read tag up to ':' or whitespace */
    size_t i = 0;
    while (cursor[i] && cursor[i] != ':' && !isspace((unsigned char)cursor[i]) &&
           i + 1 < out_size) {
        out_tag[i] = cursor[i];
        i++;
    }
    out_tag[i] = '\0';
}

static void cap_diagnose_store_line(const char *line)
{
    if (!s_diag.entries || !s_diag.capacity) {
        return;
    }
    if (!cap_diagnose_lock()) {
        return;
    }

    cap_diagnose_log_entry_t *slot = &s_diag.entries[s_diag.write_pos];
    slot->timestamp_us = esp_timer_get_time();
    slot->level = cap_diagnose_level_from_prefix(line);
    cap_diagnose_parse_tag(line, slot->tag, sizeof(slot->tag));
    strlcpy(slot->message, line, sizeof(slot->message));
    /* Trim trailing newline for cleaner JSON output. */
    size_t mlen = strlen(slot->message);
    while (mlen > 0 && (slot->message[mlen - 1] == '\n' || slot->message[mlen - 1] == '\r')) {
        slot->message[--mlen] = '\0';
    }

    s_diag.write_pos = (s_diag.write_pos + 1) % s_diag.capacity;
    if (s_diag.count < s_diag.capacity) {
        s_diag.count++;
    }

    cap_diagnose_unlock();
}

static int cap_diagnose_vprintf_hook(const char *fmt, va_list args)
{
    /* Always forward to the previous handler so the UART output is unchanged. */
    int written = 0;
    vprintf_like_t prev = s_diag.prev_vprintf;
    va_list copy;
    va_copy(copy, args);
    if (prev) {
        written = prev(fmt, args);
    } else {
        written = vprintf(fmt, args);
    }
    if (s_diag_in_hook) {
        va_end(copy);
        return written;
    }
    s_diag_in_hook = true;

    /* Stack-format the same line for our ring buffer. Length should be enough
     * for typical IDF log lines; we truncate if it overflows. */
    char buf[CONFIG_CLAW_CAP_DIAGNOSE_MAX_LINE_LEN];
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0) {
        cap_diagnose_store_line(buf);
    }

    s_diag_in_hook = false;
    return written;
}

esp_err_t cap_diagnose_start(void)
{
    if (s_diag.installed) {
        return ESP_OK;
    }
    s_diag.capacity = CONFIG_CLAW_CAP_DIAGNOSE_RING_CAPACITY;
    s_diag.entries = calloc(s_diag.capacity, sizeof(*s_diag.entries));
    if (!s_diag.entries) {
        return ESP_ERR_NO_MEM;
    }
    s_diag.mutex = xSemaphoreCreateMutex();
    if (!s_diag.mutex) {
        free(s_diag.entries);
        s_diag.entries = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* TODO: re-enable the log hook once we identify the spinlock-release
     * regression observed when esp_log_set_vprintf intercepts the default
     * event loop's log lines. For now the ring stays empty and the cap
     * still surfaces system_state for the LLM. */
    s_diag.prev_vprintf = NULL;
    s_diag.installed = true;
    ESP_LOGI(TAG, "log ring allocated (capacity=%u, hook disabled)",
             (unsigned)s_diag.capacity);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* JSON helpers                                                                */
/* ────────────────────────────────────────────────────────────────────────── */

static char cap_diagnose_normalize_level(const char *value)
{
    if (!value || !value[0]) {
        return '\0';
    }
    if (!strcasecmp(value, "error") || !strcasecmp(value, "err") ||
        !strcasecmp(value, "e")) {
        return 'E';
    }
    if (!strcasecmp(value, "warn") || !strcasecmp(value, "warning") ||
        !strcasecmp(value, "w")) {
        return 'W';
    }
    if (!strcasecmp(value, "info") || !strcasecmp(value, "i")) {
        return 'I';
    }
    if (!strcasecmp(value, "debug") || !strcasecmp(value, "d")) {
        return 'D';
    }
    return '\0';
}

static bool cap_diagnose_level_passes_filter(char actual, char minimum)
{
    /* Severity order: E > W > I > D > V (others). */
    static const char order[] = "EWIDV";
    const char *a = strchr(order, actual);
    const char *m = strchr(order, minimum);
    if (!a) {
        return false;
    }
    if (!m) {
        return true;
    }
    return (a - order) <= (m - order);
}

static esp_err_t cap_diagnose_render_logs(int limit,
                                          char min_level,
                                          const char *tag_filter,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }

    if (!cap_diagnose_lock()) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_FAIL;
    }

    int emitted = 0;
    /* Walk newest-first; ring write_pos points to the next slot. */
    for (size_t i = 0; i < s_diag.count && (limit <= 0 || emitted < limit); i++) {
        size_t idx = (s_diag.write_pos + s_diag.capacity - 1 - i) % s_diag.capacity;
        const cap_diagnose_log_entry_t *e = &s_diag.entries[idx];
        if (min_level && !cap_diagnose_level_passes_filter(e->level, min_level)) {
            continue;
        }
        if (tag_filter && tag_filter[0] && strcasecmp(e->tag, tag_filter) != 0) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        cJSON_AddNumberToObject(item, "ts_us", (double)e->timestamp_us);
        char level_str[2] = { e->level ? e->level : '?', '\0' };
        cJSON_AddStringToObject(item, "level", level_str);
        cJSON_AddStringToObject(item, "tag", e->tag);
        cJSON_AddStringToObject(item, "message", e->message);
        cJSON_AddItemToArray(arr, item);
        emitted++;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "captured", (double)s_diag.count);
    cJSON_AddNumberToObject(root, "capacity", (double)s_diag.capacity);
    cJSON_AddNumberToObject(root, "returned", (double)emitted);
    cJSON_AddItemToObject(root, "entries", arr);

    cap_diagnose_unlock();

    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Descriptor implementations                                                  */
/* ────────────────────────────────────────────────────────────────────────── */

static esp_err_t cap_diagnose_execute_logs(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    (void)ctx;
    int limit = 20;
    char min_level = 'W';   /* warn+ by default keeps the noise out */
    const char *tag_filter = NULL;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (cJSON_IsObject(root)) {
        cJSON *limit_node = cJSON_GetObjectItemCaseSensitive(root, "limit");
        if (cJSON_IsNumber(limit_node)) {
            limit = (int)limit_node->valuedouble;
            if (limit <= 0) {
                limit = 20;
            }
            if (limit > (int)s_diag.capacity) {
                limit = (int)s_diag.capacity;
            }
        }
        cJSON *level_node = cJSON_GetObjectItemCaseSensitive(root, "level");
        if (cJSON_IsString(level_node)) {
            char lvl = cap_diagnose_normalize_level(level_node->valuestring);
            if (lvl) {
                min_level = lvl;
            }
        }
        cJSON *tag_node = cJSON_GetObjectItemCaseSensitive(root, "tag");
        if (cJSON_IsString(tag_node) && tag_node->valuestring && tag_node->valuestring[0]) {
            tag_filter = tag_node->valuestring;
        }
    }

    esp_err_t err = cap_diagnose_render_logs(limit, min_level, tag_filter,
                                             output, output_size);
    cJSON_Delete(root);
    return err;
}

static const char *cap_diagnose_reset_reason_to_string(esp_reset_reason_t reason)
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

/* ────────────────────────────────────────────────────────────────────────── */
/* Task introspection (CPU% computed from two samples)                         */
/* ────────────────────────────────────────────────────────────────────────── */

#define CAP_DIAGNOSE_MAX_TASKS         48
#define CAP_DIAGNOSE_TASKS_SAMPLE_MS   500
#define CAP_DIAGNOSE_TASKS_SAMPLE_MIN  100
#define CAP_DIAGNOSE_TASKS_SAMPLE_MAX  2000
#define CAP_DIAGNOSE_TASKS_DEFAULT_LIM 25

static const char *cap_diagnose_task_state_to_string(eTaskState state)
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

typedef struct {
    UBaseType_t task_number;
    uint32_t    runtime;
} cap_diagnose_runtime_entry_t;

static esp_err_t cap_diagnose_execute_tasks(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)ctx;

    int sample_ms = CAP_DIAGNOSE_TASKS_SAMPLE_MS;
    int limit = CAP_DIAGNOSE_TASKS_DEFAULT_LIM;
    cJSON *exclude_arr = NULL;

    cJSON *root_in = cJSON_Parse(input_json ? input_json : "{}");
    if (cJSON_IsObject(root_in)) {
        cJSON *node = cJSON_GetObjectItemCaseSensitive(root_in, "sample_ms");
        if (cJSON_IsNumber(node)) {
            sample_ms = (int)node->valuedouble;
            if (sample_ms < CAP_DIAGNOSE_TASKS_SAMPLE_MIN) sample_ms = CAP_DIAGNOSE_TASKS_SAMPLE_MIN;
            if (sample_ms > CAP_DIAGNOSE_TASKS_SAMPLE_MAX) sample_ms = CAP_DIAGNOSE_TASKS_SAMPLE_MAX;
        }
        node = cJSON_GetObjectItemCaseSensitive(root_in, "limit");
        if (cJSON_IsNumber(node)) {
            limit = (int)node->valuedouble;
            if (limit <= 0) limit = CAP_DIAGNOSE_TASKS_DEFAULT_LIM;
            if (limit > CAP_DIAGNOSE_MAX_TASKS) limit = CAP_DIAGNOSE_MAX_TASKS;
        }
        /* exclude: array of task names to skip. Kept as a cJSON reference so
         * the lookup runs in O(n_excludes) per task without extra allocations. */
        exclude_arr = cJSON_GetObjectItemCaseSensitive(root_in, "exclude");
        if (!cJSON_IsArray(exclude_arr)) {
            exclude_arr = NULL;
        }
    }

    TaskHandle_t caller_handle = xTaskGetCurrentTaskHandle();

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        cJSON_Delete(root_in);
        return ESP_FAIL;
    }
    if (task_count > CAP_DIAGNOSE_MAX_TASKS) {
        task_count = CAP_DIAGNOSE_MAX_TASKS;
    }

    TaskStatus_t *first = calloc(task_count, sizeof(*first));
    TaskStatus_t *second = calloc(task_count, sizeof(*second));
    if (!first || !second) {
        free(first);
        free(second);
        cJSON_Delete(root_in);
        return ESP_ERR_NO_MEM;
    }

    uint32_t total_first = 0;
    uint32_t total_second = 0;
    UBaseType_t n_first = uxTaskGetSystemState(first, task_count, &total_first);
    vTaskDelay(pdMS_TO_TICKS(sample_ms));
    UBaseType_t n_second = uxTaskGetSystemState(second, task_count, &total_second);

    uint32_t total_delta = (total_second >= total_first) ? (total_second - total_first) : 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        cJSON_Delete(root_in);
        free(first);
        free(second);
        return ESP_ERR_NO_MEM;
    }

    /* Walk the second (newest) sample so deleted/finished tasks drop out. */
    int emitted = 0;
    for (UBaseType_t i = 0; i < n_second && emitted < limit; i++) {
        const TaskStatus_t *t = &second[i];
        const char *task_name = t->pcTaskName ? t->pcTaskName : "?";

        /* Optional exclude filter: skip if the LLM passed an exclude array
         * containing this task name (case-sensitive match). */
        if (exclude_arr) {
            bool skip = false;
            cJSON *ex_item = NULL;
            cJSON_ArrayForEach(ex_item, exclude_arr) {
                if (cJSON_IsString(ex_item) && ex_item->valuestring &&
                        strcmp(ex_item->valuestring, task_name) == 0) {
                    skip = true;
                    break;
                }
            }
            if (skip) {
                continue;
            }
        }

        uint32_t prev_runtime = 0;
        for (UBaseType_t j = 0; j < n_first; j++) {
            if (first[j].xTaskNumber == t->xTaskNumber) {
                prev_runtime = first[j].ulRunTimeCounter;
                break;
            }
        }
        double cpu_pct = 0.0;
        if (total_delta > 0 && t->ulRunTimeCounter >= prev_runtime) {
            uint32_t task_delta = t->ulRunTimeCounter - prev_runtime;
            cpu_pct = ((double)task_delta / (double)total_delta) * 100.0;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        cJSON_AddStringToObject(item, "name", task_name);
        cJSON_AddStringToObject(item, "state", cap_diagnose_task_state_to_string(t->eCurrentState));
        cJSON_AddNumberToObject(item, "priority", (double)t->uxCurrentPriority);
        cJSON_AddNumberToObject(item, "stack_free_bytes",
                                (double)t->usStackHighWaterMark * sizeof(StackType_t));
        cJSON_AddNumberToObject(item, "cpu_pct", cpu_pct);
        /* Mark the task currently running this tool call so the agent knows
         * its own CPU% is biased by the act of asking. */
        if (caller_handle && t->xHandle == caller_handle) {
            cJSON_AddBoolToObject(item, "caller", true);
        }
#if ( configTASKLIST_INCLUDE_COREID == 1 )
        cJSON_AddNumberToObject(item, "core_id",
                                t->xCoreID == tskNO_AFFINITY ? -1 : (double)t->xCoreID);
#endif
        cJSON_AddItemToArray(arr, item);
        emitted++;
    }

    /* Sort by cpu_pct descending so the LLM sees the heaviest tasks first
     * without having to sort itself. Simple insertion sort over cJSON children. */
    cJSON *sorted = cJSON_CreateArray();
    if (sorted) {
        while (cJSON_GetArraySize(arr) > 0) {
            int best_idx = 0;
            double best_pct = -1.0;
            int n = cJSON_GetArraySize(arr);
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *pct = cJSON_GetObjectItemCaseSensitive(item, "cpu_pct");
                double v = cJSON_IsNumber(pct) ? pct->valuedouble : 0.0;
                if (v > best_pct) {
                    best_pct = v;
                    best_idx = i;
                }
            }
            cJSON *removed = cJSON_DetachItemFromArray(arr, best_idx);
            if (removed) {
                cJSON_AddItemToArray(sorted, removed);
            }
        }
        cJSON_Delete(arr);
        arr = sorted;
    }

    /* Aggregate "non-idle" CPU% over the same sample window so this single
     * tool answers both "which task is busy?" and "how busy is the device
     * overall?" without needing get_cpu_usage. Idle tasks are named "IDLE",
     * "IDLE0", "IDLE1" depending on FreeRTOS variant. */
    uint32_t idle_delta = 0;
    for (UBaseType_t i = 0; i < n_second; i++) {
        const char *nm = second[i].pcTaskName;
        if (!nm) continue;
        if (strncmp(nm, "IDLE", 4) != 0) continue;
        uint32_t prev = 0;
        for (UBaseType_t j = 0; j < n_first; j++) {
            if (first[j].xTaskNumber == second[i].xTaskNumber) {
                prev = first[j].ulRunTimeCounter;
                break;
            }
        }
        if (second[i].ulRunTimeCounter >= prev) {
            idle_delta += second[i].ulRunTimeCounter - prev;
        }
    }
    double cpu_total_pct = 0.0;
    if (total_delta > 0 && idle_delta <= total_delta) {
        cpu_total_pct = 100.0 - (((double)idle_delta * 100.0) / (double)total_delta);
        if (cpu_total_pct < 0.0) cpu_total_pct = 0.0;
        if (cpu_total_pct > 100.0) cpu_total_pct = 100.0;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "sample_ms", (double)sample_ms);
    cJSON_AddNumberToObject(root, "cpu_total_pct", cpu_total_pct);
    cJSON_AddNumberToObject(root, "task_count_total", (double)uxTaskGetNumberOfTasks());
    cJSON_AddNumberToObject(root, "returned", (double)emitted);
    cJSON_AddItemToObject(root, "tasks", arr);

    free(first);
    free(second);

    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cJSON_Delete(root_in);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_diagnose_execute_system_state(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000LL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "task_count", (double)uxTaskGetNumberOfTasks());
    cJSON_AddStringToObject(root, "reset_reason",
                            cap_diagnose_reset_reason_to_string(esp_reset_reason()));
    cJSON_AddNumberToObject(root, "logs_captured", (double)s_diag.count);
    cJSON_AddNumberToObject(root, "logs_capacity", (double)s_diag.capacity);

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    cJSON_AddNumberToObject(root, "cpu_cores", chip.cores);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);

    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Descriptor registry                                                         */
/* ────────────────────────────────────────────────────────────────────────── */

static const claw_cap_descriptor_t s_diagnose_descriptors[] = {
    {
        .id = "diagnose_logs_recent",
        .name = "diagnose_logs_recent",
        .family = "diagnose",
        .description = "Return the N most recent ESP_LOG lines captured by the device. Use this when a previous tool call failed or to investigate runtime warnings.",
        .kind = CLAW_CAP_KIND_HYBRID,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"limit\":{\"type\":\"integer\",\"description\":\"Max entries to return (default 20)\"},"
            "\"level\":{\"type\":\"string\",\"enum\":[\"error\",\"warn\",\"info\",\"debug\"],\"description\":\"Minimum severity; default warn\"},"
            "\"tag\":{\"type\":\"string\",\"description\":\"Filter by ESP_LOG tag (e.g. cap_scheduler)\"}"
            "}}",
        .execute = cap_diagnose_execute_logs,
    },
    {
        .id = "diagnose_system_state",
        .name = "diagnose_system_state",
        .family = "diagnose",
        .description = "Return a compact system health snapshot: uptime, free heap, task count, last reset reason.",
        .kind = CLAW_CAP_KIND_HYBRID,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_diagnose_execute_system_state,
    },
    {
        .id = "list_top_cpu_tasks",
        .name = "list_top_cpu_tasks",
        .family = "diagnose",
        .description = "Lists running FreeRTOS tasks with per-task CPU%, state, priority, stack free, and core. Sorted by CPU% descending. Output also includes cpu_total_pct (overall non-idle ratio). A task may carry caller=true: that is the agent worker running this very call, so its CPU% is inflated by the act of asking — to see what else is busy, re-call with exclude=[\"<caller_name>\"]. Use this for any per-task CPU/process question. Blocks briefly (sample_ms, default 500ms) to compute deltas.",
        .kind = CLAW_CAP_KIND_HYBRID,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"sample_ms\":{\"type\":\"integer\",\"description\":\"Window between samples in ms (100-2000, default 500). Longer = more accurate CPU%.\"},"
            "\"limit\":{\"type\":\"integer\",\"description\":\"Max number of task entries to return (default 25).\"},"
            "\"exclude\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional list of task names to skip. Useful to filter out the caller task (the one marked caller=true) to see what else is busy.\"}"
            "}}",
        .execute = cap_diagnose_execute_tasks,
    },
};

static const claw_cap_group_t s_diagnose_group = {
    .group_id = "cap_diagnose",
    .descriptors = s_diagnose_descriptors,
    .descriptor_count = sizeof(s_diagnose_descriptors) / sizeof(s_diagnose_descriptors[0]),
};

esp_err_t cap_diagnose_register_group(void)
{
    if (claw_cap_group_exists(s_diagnose_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_diagnose_group);
}
