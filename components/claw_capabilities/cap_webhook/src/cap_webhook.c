/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_webhook.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_webhook";

#define CAP_WEBHOOK_MAX                  16
#define CAP_WEBHOOK_NAME_LEN             48
#define CAP_WEBHOOK_DESC_LEN             160
#define CAP_WEBHOOK_URL_LEN              320
#define CAP_WEBHOOK_METHOD_LEN           8
#define CAP_WEBHOOK_RESP_PREVIEW_BYTES   512

typedef struct {
    char name[CAP_WEBHOOK_NAME_LEN];
    char description[CAP_WEBHOOK_DESC_LEN];
    char url[CAP_WEBHOOK_URL_LEN];
    char method[CAP_WEBHOOK_METHOD_LEN];
} cap_webhook_entry_t;

typedef struct {
    cap_webhook_entry_t entries[CAP_WEBHOOK_MAX];
    size_t count;
    SemaphoreHandle_t lock;
} cap_webhook_registry_t;

static cap_webhook_registry_t s_registry = {0};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_webhook_resp_buf_t;

static esp_err_t cap_webhook_lock(void)
{
    if (!s_registry.lock) {
        s_registry.lock = xSemaphoreCreateMutex();
        if (!s_registry.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return xSemaphoreTake(s_registry.lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void cap_webhook_unlock(void)
{
    if (s_registry.lock) {
        xSemaphoreGive(s_registry.lock);
    }
}

static const char *cap_webhook_pick_string(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(node) && node->valuestring && node->valuestring[0]) {
        return node->valuestring;
    }
    return fallback;
}

esp_err_t cap_webhook_set_registry(const char *json)
{
    if (cap_webhook_lock() != ESP_OK) {
        return ESP_FAIL;
    }
    s_registry.count = 0;

    if (!json || !json[0]) {
        cap_webhook_unlock();
        ESP_LOGI(TAG, "Webhook registry cleared");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        cap_webhook_unlock();
        ESP_LOGW(TAG, "Invalid webhook registry JSON; expected array");
        return ESP_ERR_INVALID_ARG;
    }

    const int total = cJSON_GetArraySize(root);
    for (int i = 0; i < total && s_registry.count < CAP_WEBHOOK_MAX; ++i) {
        const cJSON *item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        const char *name = cap_webhook_pick_string(item, "name", NULL);
        const char *url  = cap_webhook_pick_string(item, "url", NULL);
        if (!name || !url) {
            continue;
        }
        const char *method = cap_webhook_pick_string(item, "method", "POST");
        const char *desc   = cap_webhook_pick_string(item, "description", "");

        cap_webhook_entry_t *slot = &s_registry.entries[s_registry.count];
        memset(slot, 0, sizeof(*slot));
        strlcpy(slot->name, name, sizeof(slot->name));
        strlcpy(slot->url, url, sizeof(slot->url));
        strlcpy(slot->method, method, sizeof(slot->method));
        strlcpy(slot->description, desc, sizeof(slot->description));
        s_registry.count++;
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Webhook registry loaded: %u entries", (unsigned)s_registry.count);
    cap_webhook_unlock();
    return ESP_OK;
}

static esp_err_t cap_webhook_execute_list(const char *args_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    (void)args_json;
    (void)ctx;

    if (cap_webhook_lock() != ESP_OK) {
        snprintf(output, output_size, "Error: failed to lock webhook registry");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "webhooks");
    for (size_t i = 0; i < s_registry.count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", s_registry.entries[i].name);
        cJSON_AddStringToObject(item, "method", s_registry.entries[i].method);
        if (s_registry.entries[i].description[0]) {
            cJSON_AddStringToObject(item, "description", s_registry.entries[i].description);
        }
        cJSON_AddItemToArray(arr, item);
    }
    cap_webhook_unlock();

    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) {
        snprintf(output, output_size, "Error: failed to serialize webhook list");
        return ESP_FAIL;
    }
    strlcpy(output, serialized, output_size);
    free(serialized);
    return ESP_OK;
}

static esp_err_t cap_webhook_http_event(esp_http_client_event_t *event)
{
    if (!event || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    cap_webhook_resp_buf_t *buf = (cap_webhook_resp_buf_t *)event->user_data;
    if (!buf || !buf->data) {
        return ESP_OK;
    }
    size_t available = buf->cap > 0 ? buf->cap - 1 - buf->len : 0;
    if (available == 0) {
        return ESP_OK;
    }
    size_t copy = (size_t)event->data_len < available ? (size_t)event->data_len : available;
    memcpy(buf->data + buf->len, event->data, copy);
    buf->len += copy;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static esp_err_t cap_webhook_execute_trigger(const char *args_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    (void)ctx;

    if (!args_json) {
        snprintf(output, output_size, "Error: missing arguments");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *args = cJSON_Parse(args_json);
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: invalid arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *name_node = cJSON_GetObjectItemCaseSensitive(args, "name");
    const char *name = cJSON_IsString(name_node) ? name_node->valuestring : NULL;
    if (!name || !name[0]) {
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: 'name' is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolve webhook in registry under lock, copy fields to local stack */
    cap_webhook_entry_t entry = {0};
    bool found = false;
    if (cap_webhook_lock() != ESP_OK) {
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: failed to lock webhook registry");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < s_registry.count; ++i) {
        if (strcmp(s_registry.entries[i].name, name) == 0) {
            entry = s_registry.entries[i];
            found = true;
            break;
        }
    }
    cap_webhook_unlock();

    if (!found) {
        cJSON_Delete(args);
        snprintf(output, output_size,
                 "Error: webhook '%s' is not in the registry. Use webhook_list to see available names.",
                 name);
        return ESP_ERR_NOT_FOUND;
    }

    /* Serialize the optional payload object into a JSON body */
    const cJSON *payload_node = cJSON_GetObjectItemCaseSensitive(args, "payload");
    char *body = NULL;
    if (payload_node && !cJSON_IsNull(payload_node)) {
        body = cJSON_PrintUnformatted(payload_node);
    }

    /* Resolve HTTP method */
    esp_http_client_method_t method = HTTP_METHOD_POST;
    if (strcasecmp(entry.method, "GET") == 0) {
        method = HTTP_METHOD_GET;
    } else if (strcasecmp(entry.method, "PUT") == 0) {
        method = HTTP_METHOD_PUT;
    } else if (strcasecmp(entry.method, "PATCH") == 0) {
        method = HTTP_METHOD_PATCH;
    }

    cap_webhook_resp_buf_t resp = {0};
    resp.cap = CAP_WEBHOOK_RESP_PREVIEW_BYTES;
    resp.data = malloc(resp.cap);
    if (!resp.data) {
        free(body);
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = entry.url,
        .method = method,
        .timeout_ms = 8000,
        .event_handler = cap_webhook_http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp.data);
        free(body);
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    if (body && method != HTTP_METHOD_GET) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    /* Build a structured JSON result for the LLM */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", entry.name);
    cJSON_AddStringToObject(result, "method", entry.method);
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK && status_code >= 200 && status_code < 300);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(result, "status", status_code);
    } else {
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
    }
    if (resp.len > 0) {
        cJSON_AddStringToObject(result, "response_preview", resp.data);
    }

    char *serialized = cJSON_PrintUnformatted(result);
    if (serialized) {
        strlcpy(output, serialized, output_size);
        free(serialized);
    } else {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"serialize_failed\"}");
    }
    cJSON_Delete(result);

    free(resp.data);
    free(body);
    cJSON_Delete(args);
    return err;
}

static const claw_cap_descriptor_t s_webhook_descriptors[] = {
    {
        .id = "webhook_list",
        .name = "webhook_list",
        .family = "system",
        .description = "List the webhooks that the user has pre-configured for outbound HTTP notifications. "
                       "The LLM cannot register new webhooks here \xE2\x80\x94 they must be added through the device UI.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_webhook_execute_list,
    },
    {
        .id = "webhook_trigger",
        .name = "webhook_trigger",
        .family = "system",
        .description = "Trigger one of the user-configured webhooks by name. Optionally include a JSON 'payload' "
                       "that will be sent as the request body for non-GET methods.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"name\":{\"type\":\"string\",\"description\":\"Webhook name from webhook_list\"},"
              "\"payload\":{\"type\":\"object\",\"description\":\"Optional JSON body for the request\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute = cap_webhook_execute_trigger,
    },
};

static const claw_cap_group_t s_webhook_group = {
    .group_id = "cap_webhook",
    .descriptors = s_webhook_descriptors,
    .descriptor_count = sizeof(s_webhook_descriptors) / sizeof(s_webhook_descriptors[0]),
};

esp_err_t cap_webhook_register_group(void)
{
    if (claw_cap_group_exists(s_webhook_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_webhook_group);
}
