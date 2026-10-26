#include "sdkconfig.h"
#if CONFIG_MUSE_HTTP_TTS
#include "muse_tts_http.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "muse_tts_http";
struct request_t { char *body; uint32_t generation; int message; size_t capacity; uint32_t ticket; };
static QueueHandle_t s_requests, s_results;
static bool s_busy;
static std::atomic<uint32_t> s_ticket{0};
struct download_t {
    uint8_t *audio;
    size_t length, capacity;
    uint32_t ticket;
    int64_t deadline;
    bool failed, audio_type;
};

static esp_err_t on_http(esp_http_client_event_t *event)
{
    auto *d = static_cast<download_t *>(event->user_data);
    if (d->ticket != s_ticket.load() || esp_timer_get_time() > d->deadline) {
        d->failed = true;
        return ESP_FAIL;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        !strcasecmp(event->header_key, "Content-Type")) {
        const char *v = event->header_value;
        d->audio_type = !strncasecmp(v, "audio/mpeg", 10) && (v[10] == 0 || v[10] == ';');
    }
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (event->data_len < 0 || static_cast<size_t>(event->data_len) > d->capacity - d->length) {
            d->failed = true;
            return ESP_FAIL;
        }
        if (esp_http_client_get_status_code(event->client) != 200 || !d->audio_type) {
            d->failed = true;
            return ESP_FAIL;
        }
        memcpy(d->audio + d->length, event->data, event->data_len);
        d->length += event->data_len;
    }
    return ESP_OK;
}

static void worker(void *)
{
    request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) continue;
        muse_tts_result_t result{request.generation, request.message, nullptr, 0};
        download_t d{};
        d.capacity = request.capacity;
        d.ticket = request.ticket;
        d.deadline = esp_timer_get_time() + 30000000LL;
        d.audio = static_cast<uint8_t *>(heap_caps_malloc(d.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        esp_http_client_config_t config{};
        config.url = CONFIG_MUSE_HTTP_TTS_URL;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 25000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true; // Never forward the key to another endpoint.
        config.event_handler = on_http;
        config.user_data = &d;
        auto client = d.audio ? esp_http_client_init(&config) : nullptr;
        if (client && request.ticket == s_ticket.load()) {
            bool configured = esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK &&
                esp_http_client_set_header(client, "Accept", "audio/mpeg") == ESP_OK &&
                esp_http_client_set_header(client, "x-api-key", CONFIG_MUSE_HTTP_TTS_API_KEY) == ESP_OK &&
                esp_http_client_set_post_field(client, request.body, strlen(request.body)) == ESP_OK;
            esp_err_t err = configured ? esp_http_client_perform(client) : ESP_FAIL;
            int status = esp_http_client_get_status_code(client);
            if (err == ESP_OK && status == 200 && !d.failed && d.audio_type && d.length &&
                request.ticket == s_ticket.load() && esp_http_client_is_complete_data_received(client)) {
                result.audio = d.audio;
                result.length = d.length;
                d.audio = nullptr;
            } else {
                ESP_LOGW(TAG, "TTS request failed (HTTP %d, %s)", status, esp_err_to_name(err));
            }
        }
        if (client) esp_http_client_cleanup(client);
        free(d.audio);
        free(request.body);
        // One in-flight request; the owner drains this before starting another.
        xQueueSend(s_results, &result, portMAX_DELAY);
    }
}

bool muse_tts_http_start(const char *text, uint32_t generation, int message, size_t capacity)
{
    if (s_busy || !text || !text[0] || !capacity ||
        strncmp(CONFIG_MUSE_HTTP_TTS_URL, "https://", 8) || !CONFIG_MUSE_HTTP_TTS_API_KEY[0]) return false;
    if (!s_requests) {
        s_requests = xQueueCreate(1, sizeof(request_t));
        s_results = xQueueCreate(1, sizeof(muse_tts_result_t));
        if (!s_requests || !s_results || xTaskCreate(worker, "muse_tts", 8192, nullptr, 4, nullptr) != pdPASS) {
            if (s_requests) vQueueDelete(s_requests);
            if (s_results) vQueueDelete(s_results);
            s_requests = s_results = nullptr;
            return false;
        }
    }
    // Caption storage can end mid-UTF-8 sequence; trim only that partial suffix.
    char copy[1024];
    size_t n = strnlen(text, sizeof(copy) - 1);
    memcpy(copy, text, n);
    copy[n] = 0;
    if (n) {
        size_t start = n - 1;
        while (start && (static_cast<unsigned char>(copy[start]) & 0xc0) == 0x80) --start;
        unsigned char lead = copy[start];
        size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (n - start < width) copy[start] = 0;
    }
    // The API accepts at most 1,000 Unicode characters (not UTF-8 bytes).
    size_t chars = 0;
    for (size_t j = 0; copy[j]; ++j) {
        if ((static_cast<unsigned char>(copy[j]) & 0xc0) != 0x80 && ++chars > 1000) {
            copy[j] = 0;
            break;
        }
    }
    cJSON *json = cJSON_CreateObject();
    if (!json) return false;
    bool added = cJSON_AddStringToObject(json, "text", copy) != nullptr;
    char *body = added ? cJSON_PrintUnformatted(json) : nullptr;
    cJSON_Delete(json);
    if (!body) return false;
    request_t request{body, generation, message, capacity, ++s_ticket};
    if (xQueueSend(s_requests, &request, 0) != pdTRUE) { free(body); return false; }
    s_busy = true;
    return true;
}

bool muse_tts_http_poll(muse_tts_result_t *result)
{
    if (!s_results || xQueueReceive(s_results, result, 0) != pdTRUE) return false;
    s_busy = false;
    return true;
}

void muse_tts_http_cancel(void) { ++s_ticket; }
#endif
