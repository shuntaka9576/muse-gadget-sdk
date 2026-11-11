/* Echo reachability check over the paired device's existing Link session. */
#include "muse_vm_probe.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_link.h"
#include "muse_state.h"
#include "muse_wifi.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Static storage remains valid after timeout/cancel. Generation rejects late callbacks. */
static struct {
    uintptr_t generation;
    int status;
    bool done, overflow;
    size_t len;
    char body[512];
} s_rx;

static void report(const char *text)
{
    muse_state_set_caption("%s", text);
    ESP_LOGI("muse_vm_probe", "%s", text);
}

static void receive(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    portENTER_CRITICAL(&s_lock);
    if ((uintptr_t)ctx == s_rx.generation && !s_rx.done) {
        if (status) s_rx.status = status;
        if (len > sizeof(s_rx.body) - 1 - s_rx.len) s_rx.overflow = true;
        else if (len && !s_rx.overflow) {
            memcpy(s_rx.body + s_rx.len, data, len);
            s_rx.len += len;
            s_rx.body[s_rx.len] = 0;
        }
        s_rx.done = end || status < 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_vm_probe_run(char *result, size_t cap, bool (*cancelled)(void))
{
    const char *out = "ECHO CANCELLED";
    int64_t stream = 0;
    if (!muse_link_hatch_linked()) { out = "PAIR WITH MUSE FIRST"; goto finish; }
    if (!muse_wifi_connected()) { out = "NO WI-FI"; goto finish; }
    report("VM INFO: CHECKING");
    char id[128], name[128];
    char *token = NULL;
    bool found = muse_link_hatch_vm("", id, sizeof(id), name, sizeof(name), &token);
    /* The lookup owns refresh handling; the probe never logs or persists its token. */
    if (token) { memset(token, 0, strlen(token)); free(token); }
    if (!found) { out = "VM INFO: FAILED"; goto finish; }
    if (cancelled()) goto finish;
    report("VM INFO: OK / NOISE: WAIT");
    int64_t deadline = esp_timer_get_time() + 20000000;
    while (!muse_link_req_ready()) {
        if (cancelled()) goto finish;
        if (esp_timer_get_time() >= deadline) { out = "VM INFO OK / NOISE FAILED"; goto finish; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    report("VM INFO OK / NOISE OK / ECHO WAIT");
    uint8_t random[16];
    char nonce[33], path[64];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(nonce + i * 2, 3, "%02x", random[i]);
    snprintf(path, sizeof(path), "/probe/echo?nonce=%s", nonce);
    portENTER_CRITICAL(&s_lock);
    uintptr_t generation = ++s_rx.generation;
    s_rx.status = 0; s_rx.done = s_rx.overflow = false; s_rx.len = 0; s_rx.body[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    stream = muse_link_req_open("GET", path, NULL, true, receive, (void *)generation);
    if (!stream) { out = "NOISE OK / ECHO OPEN FAILED"; goto finish; }
    deadline = esp_timer_get_time() + 15000000;
    for (;;) {
        if (cancelled()) goto finish;
        portENTER_CRITICAL(&s_lock);
        bool done = s_rx.done;
        portEXIT_CRITICAL(&s_lock);
        if (done) break;
        if (esp_timer_get_time() >= deadline) { out = "NOISE OK / ECHO TIMEOUT"; goto finish; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* A terminal callback has finished writing. No other task starts a probe. */
    if (s_rx.status != 200) {
        snprintf(result, cap, "NOISE OK / ECHO HTTP %d", s_rx.status);
        if (s_rx.status < 0) snprintf(result, cap, "ECHO TRANSPORT FAILED");
        out = NULL;
    } else if (s_rx.overflow) out = "ECHO RESPONSE TOO LARGE";
    else {
        const char *end = NULL;
        cJSON *json = cJSON_ParseWithLengthOpts(s_rx.body, s_rx.len + 1, &end, true);
        const cJSON *server = cJSON_GetObjectItemCaseSensitive(json, "server");
        const cJSON *echo = cJSON_GetObjectItemCaseSensitive(json, "nonce");
        bool ok = end == s_rx.body + s_rx.len && cJSON_IsString(server) && !strcmp(server->valuestring, "muse-vm-probe") &&
                  cJSON_IsString(echo) && !strcmp(echo->valuestring, nonce);
        out = ok ? "VM INFO OK / NOISE OK / ECHO PASS" : "ECHO RESPONSE MISMATCH";
        cJSON_Delete(json);
    }
finish:
    portENTER_CRITICAL(&s_lock);
    ++s_rx.generation;
    portEXIT_CRITICAL(&s_lock);
    if (stream) muse_link_req_cancel(stream);
    if (out) snprintf(result, cap, "%s", out);
    report(result);
}
