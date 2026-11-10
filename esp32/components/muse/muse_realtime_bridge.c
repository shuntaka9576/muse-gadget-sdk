/* StopWatch PTT transport for muse-realtime-bridge protocol v1.
 * Network ownership stays on this worker; the microphone task never waits on I/O.
 * No OpenAI credential belongs on this device. */
#include "sdkconfig.h"
#include "muse_voice_call.h"
#include "muse_voice_call_priv.h"
#include "muse_realtime_bridge_core.h"
#include "muse_wifi.h"
#include "muse_mem.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_DEPTH 300 /* six seconds of 20 ms input, including connect time */
#define OUTPUT_FRAMES (16000 * 30)
#define RX_DEPTH 8
#define PCM_CHUNK 320

typedef enum { CMD_DOWN, CMD_AUDIO, CMD_UP } command_kind_t;
typedef struct {
    command_kind_t kind;
    unsigned epoch;
    size_t frames;
    int16_t pcm[PCM_CHUNK];
    char item[MUSE_RB_ID_MAX];
    unsigned played_ms;
} command_t;

typedef struct {
    esp_websocket_client_handle_t ws;
    muse_rb_rx_t rx;
    char assembly[MUSE_RB_MESSAGE_MAX];
    muse_vc_uplink_t up;
    muse_vc_resampler_t down;
    muse_rb_turn_t turn;
    char line[MUSE_VC_APPEND_MAX];
    unsigned epoch;
    size_t captured;
    bool live;
    int64_t next_send;
    char item[MUSE_RB_ID_MAX];
    char truncated[MUSE_RB_ID_MAX];
} worker_t;

static QueueHandle_t s_input, s_rx;
static StaticQueue_t s_input_ctl;
static SemaphoreHandle_t s_lock;
static int16_t *s_output;
static size_t s_head, s_count;
static unsigned s_epoch;
static bool s_active, s_held, s_flushed, s_ready_event, s_end_event;
static muse_voice_call_ptt_status_t s_status;
static char s_caption[192];
static bool s_caption_event;
static char s_item[MUSE_RB_ID_MAX];
static char s_cut_item[MUSE_RB_ID_MAX];
static unsigned s_cut_ms;
static uint64_t s_written, s_wire_samples;
static int64_t s_last_write;
static atomic_bool s_stop, s_failed;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static bool failed(void) { return atomic_load(&s_stop) || atomic_load(&s_failed); }
static void fail(void) { atomic_store(&s_failed, true); }

bool muse_voice_call_init(void)
{
    if (s_lock) return true;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    void *input = heap_caps_calloc(INPUT_DEPTH, sizeof(command_t), MUSE_BIG_CAPS);
    int16_t *output = heap_caps_calloc(OUTPUT_FRAMES, sizeof(int16_t), MUSE_BIG_CAPS);
    QueueHandle_t rx = xQueueCreate(RX_DEPTH, sizeof(char *));
    if (!mutex || !input || !output || !rx) {
        if (mutex) vSemaphoreDelete(mutex);
        if (rx) vQueueDelete(rx);
        free(input); free(output);
        return false;
    }
    s_input = xQueueCreateStatic(INPUT_DEPTH, sizeof(command_t), input, &s_input_ctl);
    s_rx = rx;
    s_output = output;
    s_lock = mutex;
    return true;
}

bool muse_voice_call_ready(void)
{
    return s_lock && muse_wifi_connected() &&
        muse_rb_config_valid(CONFIG_MUSE_REALTIME_BRIDGE_URL, CONFIG_MUSE_REALTIME_BRIDGE_TOKEN);
}

static void socket_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)base;
    worker_t *w = arg;
    if (failed()) return; /* Closing a locally ended call is not a failure. */
    if (event == WEBSOCKET_EVENT_ERROR || event == WEBSOCKET_EVENT_DISCONNECTED) {
        fail();
        return;
    }
    if (event != WEBSOCKET_EVENT_DATA || failed()) return;
    esp_websocket_event_data_t *e = data;
    if (e->op_code == 8) { fail(); return; }
    if (e->data_len < 0 || e->payload_len < 0 || e->payload_offset < 0) { fail(); return; }
    int result = muse_rb_rx_feed(&w->rx, w->assembly, sizeof(w->assembly), e->op_code, e->fin,
                               e->payload_len, e->payload_offset, e->data_ptr, e->data_len);
    if (result < 0) { fail(); return; }
    if (result == 1) {
        /* JSON cannot contain a literal NUL; strlen below must see all bytes. */
        if (memchr(w->assembly, 0, w->rx.used)) { fail(); return; }
        char *copy = heap_caps_malloc(w->rx.used + 1, MUSE_BIG_CAPS);
        if (!copy) { fail(); return; }
        memcpy(copy, w->assembly, w->rx.used + 1);
        if (xQueueSend(s_rx, &copy, 0) != pdTRUE) { free(copy); fail(); }
    }
}

static bool send_json(worker_t *w, const char *json)
{
    size_t n = strlen(json);
    if (failed() || esp_websocket_client_send_text(w->ws, json, n, pdMS_TO_TICKS(2000)) != (int)n) {
        fail(); return false;
    }
    return true;
}

static void truncate_item(worker_t *w, const char *item, unsigned ms)
{
    if (!item[0] || !strcmp(w->truncated,item)) return;
    char escaped[6 * MUSE_RB_ID_MAX], json[6 * MUSE_RB_ID_MAX + 128];
    if (!muse_vc_json_escape(item,escaped,sizeof(escaped))) { fail(); return; }
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.truncate\",\"item_id\":\"%s\","
             "\"content_index\":0,\"audio_end_ms\":%u}",escaped,ms);
    if (send_json(w,json)) strcpy(w->truncated,item);
}

static void receive(worker_t *w, const char *j)
{
    size_t n = strlen(j);
    char type[96], id[MUSE_RB_ID_MAX];
    if (!muse_rb_string(j,n,"type",type,sizeof(type))) { fail(); return; }
    if (!strcmp(type,"bridge.ready")) {
        if (w->live || !muse_rb_ready(j,n)) { fail(); return; }
        w->live = true;
        lock(); s_status.live = true; s_status.connecting = false; s_ready_event = true; unlock();
        return;
    }
    if (!w->live || !strcmp(type,"bridge.error") || !strcmp(type,"error")) { fail(); return; }
    if (!strcmp(type,"response.created")) {
        if (!muse_rb_string(j,n,"response.id",id,sizeof(id)) || !muse_rb_turn_created(&w->turn,id)) fail();
        return;
    }
    if (!strcmp(type,"response.done")) {
        if (!muse_rb_string(j,n,"response.id",id,sizeof(id)) || !muse_rb_turn_done(&w->turn,id)) {
            fail(); return;
        }
        lock();
        if (w->turn.requested_epoch == s_epoch) {
            s_status.reply_done = true; s_status.working = false;
        }
        unlock();
        return;
    }
    if (!strcmp(type,"response.output_audio.delta")) {
        char item[MUSE_RB_ID_MAX];
        muse_vc_span_t audio, index;
        if (!muse_rb_string(j,n,"response_id",id,sizeof(id)) ||
            !muse_rb_string(j,n,"item_id",item,sizeof(item)) || !item[0] ||
            muse_vc_json_get(j,n,"delta",&audio) != MUSE_VC_J_STRING ||
            muse_vc_json_get(j,n,"content_index",&index) != MUSE_VC_J_NUMBER ||
            index.n != 1 || index.p[0] != '0') { fail(); return; }
        lock();
        bool play = !s_held && muse_rb_turn_audio(&w->turn,s_epoch,id);
        unlock();
        if (!play) {
            lock();
            bool press_waiting = w->epoch != s_epoch;
            unsigned ms = !strcmp(s_cut_item,item) ? s_cut_ms : 0;
            unlock();
            /* DOWN owns cancel-before-truncate. Remember audio that arrived
             * just before that command, even if none was handed to playback. */
            if (press_waiting) {
                if (!w->item[0] && !strcmp(w->turn.response,id)) strcpy(w->item,item);
            } else truncate_item(w,item,ms);
            return;
        }
        /* Protocol v1 produces a single audio item per response. Fail closed
         * if that changes instead of attributing playback to the wrong item. */
        if (w->item[0] && strcmp(w->item,item)) { fail(); return; }
        strcpy(w->item,item);
        uint8_t scratch[1920]; int16_t pcm[642]; bool audible;
        long wire_bytes = muse_vc_b64_decode(audio,scratch,sizeof(scratch));
        long count = muse_vc_decode_audio(&w->down,audio,scratch,sizeof(scratch),pcm,&audible);
        if (wire_bytes < 0 || count < 0) { fail(); return; }
        lock();
        if (!s_held && muse_rb_turn_audio(&w->turn,s_epoch,id)) {
            if ((size_t)count > OUTPUT_FRAMES - s_count) fail();
            else {
                strcpy(s_item,item);
                s_wire_samples += (size_t)wire_bytes / 2;
                for (long i = 0; i < count; ++i) s_output[(s_head + s_count++) % OUTPUT_FRAMES] = pcm[i];
                s_status.reply_audio = true; s_status.reply_open = true;
            }
        }
        unlock();
    } else if (!strcmp(type,"response.output_audio_transcript.done")) {
        if (!muse_rb_string(j,n,"response_id",id,sizeof(id))) return;
        muse_vc_span_t t;
        if (muse_vc_json_get(j,n,"transcript",&t) != MUSE_VC_J_STRING) return;
        lock();
        if (!s_held && muse_rb_turn_audio(&w->turn,s_epoch,id)) {
            muse_vc_json_unescape(t,s_caption,sizeof(s_caption)); s_caption_event = true;
        }
        unlock();
    }
}

static void create_response(worker_t *w)
{
    if (!muse_rb_turn_create(&w->turn,w->epoch)) return;
    w->item[0] = 0;
    muse_vc_resampler_init(&w->down,24000,16000);
    send_json(w,"{\"type\":\"input_audio_buffer.commit\"}");
    send_json(w,"{\"type\":\"response.create\"}");
}

static void command(worker_t *w, const command_t *c)
{
    if (c->kind == CMD_DOWN) {
        muse_rb_turn_press(&w->turn);
        if (w->turn.pending) send_json(w,"{\"type\":\"response.cancel\"}");
        truncate_item(w,c->item,c->played_ms);
        if (!c->item[0]) truncate_item(w,w->item,0);
        send_json(w,"{\"type\":\"input_audio_buffer.clear\"}");
        w->epoch = c->epoch;
        w->captured = 0;
        muse_vc_uplink_init(&w->up);
        return;
    }
    lock(); bool current = c->epoch == s_epoch; unlock();
    if (!current || c->epoch != w->epoch) return;
    if (c->kind == CMD_AUDIO) {
        w->captured += c->frames;
        if (w->captured > 16000 * 60) { fail(); return; }
        size_t offset = 0;
        while (offset < c->frames && !failed()) {
            size_t bytes = 0;
            size_t used = muse_vc_uplink_push(&w->up,c->pcm + offset,c->frames - offset,w->line,&bytes);
            if (!used) { fail(); return; }
            offset += used;
            if (bytes) {
                send_json(w,w->line);
                w->next_send = esp_timer_get_time() + 40000;
            }
        }
    } else {
        if (w->captured < 1600) {
            send_json(w,"{\"type\":\"input_audio_buffer.clear\"}");
        } else {
            /* Pad only the last partial 40 ms frame. The minimum duration is
             * checked against captured input, not this padding. */
            int16_t zeros[PCM_CHUNK] = {0};
            while (w->up.fill && !failed()) {
                size_t bytes = 0;
                muse_vc_uplink_push(&w->up,zeros,PCM_CHUNK,w->line,&bytes);
                if (bytes) { send_json(w,w->line); break; }
            }
            /* Keep input uncommitted while an old response is cancelling.
             * A new press can then clear it without orphaning a commit. */
            w->turn.create_queued = true;
        }
        lock();
        if (c->epoch == s_epoch) {
            s_status.sending = false; s_status.heard = w->captured >= 1600;
            s_status.working = w->captured >= 1600;
        }
        unlock();
    }
}

static void worker(void *unused)
{
    (void)unused;
    worker_t *w = heap_caps_calloc(1,sizeof(*w),MUSE_BIG_CAPS);
    char headers[300];
    muse_wifi_keep_awake(true);
    if (!w) { fail(); goto end; }
    snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",CONFIG_MUSE_REALTIME_BRIDGE_TOKEN);
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_MUSE_REALTIME_BRIDGE_URL, .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_reconnect = true,
        .network_timeout_ms = 5000, .task_stack = 6144, .buffer_size = 4096,
    };
    w->ws = esp_websocket_client_init(&cfg);
    if (!w->ws) { fail(); goto end; }
    if (esp_websocket_register_events(w->ws,WEBSOCKET_EVENT_ANY,socket_event,w) != ESP_OK ||
        esp_websocket_client_start(w->ws) != ESP_OK) { fail(); goto end; }
    int64_t started = esp_timer_get_time();
    while (!failed()) {
        char *json;
        /* Bound each receive burst so input and hangup stay responsive. */
        for (int i = 0; i < RX_DEPTH && xQueueReceive(s_rx,&json,0) == pdTRUE; ++i) {
            receive(w,json); free(json);
            if (failed()) break;
        }
        int64_t now = esp_timer_get_time();
        if ((!w->live && now - started > 20000000) || now - started > 600000000) { fail(); break; }
        if (w->live && !failed()) {
            command_t c;
            for (int i = 0; i < INPUT_DEPTH && !failed(); ++i) {
                if (xQueuePeek(s_input,&c,0) != pdTRUE) break;
                lock(); bool stale = c.epoch != s_epoch; unlock();
                if (c.kind != CMD_DOWN && stale) {
                    xQueueReceive(s_input,&c,0);
                    continue;
                }
                if (c.kind != CMD_DOWN && esp_timer_get_time() < w->next_send) break;
                xQueueReceive(s_input,&c,0);
                command(w,&c);
                /* Revisit RX between live microphone blocks. */
                if (c.kind == CMD_AUDIO) break;
            }
            /* Don't start a superseded response while an interruption waits. */
            lock(); bool current = w->epoch == s_epoch; unlock();
            if (current && !failed()) create_response(w);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
end:
    if (w && w->ws) {
        /* No reconnect or replay after a disconnect. Destroy joins callbacks
         * before the shared receive queue or worker storage is reclaimed. */
        esp_websocket_client_stop(w->ws);
        esp_websocket_client_destroy(w->ws);
    }
    free(w);
    char *json;
    while (xQueueReceive(s_rx,&json,0) == pdTRUE) free(json);
    xQueueReset(s_input);
    muse_wifi_keep_awake(false);
    lock();
    s_active = false; s_held = false; s_count = 0; s_flushed = true;
    memset(&s_status,0,sizeof(s_status));
    s_end_event = true;
    unlock();
    vTaskDelete(NULL);
}

static void queue_command(const command_t *c)
{
    if (xQueueSend(s_input,c,0) != pdTRUE) {
        lock(); s_status.overflow = true; unlock(); fail();
    }
}

void muse_voice_call_start_ptt(void)
{
    if (!s_lock) return; /* Initialization failure is reported by ready(). */
    lock();
    if (s_active) { unlock(); return; }
    s_active = true; s_held = false; s_epoch = 0; s_head = s_count = 0;
    s_ready_event = s_end_event = s_caption_event = false;
    s_item[0] = s_cut_item[0] = 0; s_cut_ms = 0; s_written = s_wire_samples = 0; s_last_write = 0; s_flushed = true;
    memset(&s_status,0,sizeof(s_status));
    s_status.connecting = true;
    atomic_store(&s_stop,false); atomic_store(&s_failed,false);
    unlock();
    muse_voice_call_set_talking(true);
    if (!muse_voice_call_ready() || xTaskCreate(worker,"realtime_bridge",12288,NULL,5,NULL) != pdPASS) {
        lock(); s_active = false; s_end_event = true; memset(&s_status,0,sizeof(s_status)); unlock();
        xQueueReset(s_input); fail();
    }
}
void muse_voice_call_start(void) { muse_voice_call_start_ptt(); }

void muse_voice_call_set_talking(bool held)
{
    if (!s_lock) return;
    command_t c = {.kind = held ? CMD_DOWN : CMD_UP};
    lock();
    if (!s_active || s_held == held) { unlock(); return; }
    s_held = held;
    if (held) {
        ++s_epoch;
        strcpy(c.item,s_item);
        /* Conservative playout estimate: count successful writes, subtract
         * the outstanding 120 ms speaker pipeline. Never count received PCM. */
        int64_t remaining = 120000 - (esp_timer_get_time() - s_last_write);
        uint64_t queued = remaining > 0 ? (uint64_t)remaining * 16000 / 1000000 : 0;
        c.played_ms = s_written > queued ? (s_written - queued) / 16 : 0;
        if (c.played_ms > s_wire_samples / 24) c.played_ms = s_wire_samples / 24;
        strcpy(s_cut_item,c.item); s_cut_ms = c.played_ms;
        s_item[0] = 0; s_written = s_wire_samples = 0;
        s_head = s_count = 0; s_flushed = true; s_caption_event = false;
        bool live = s_status.live;
        memset(&s_status,0,sizeof(s_status));
        s_status.live = live; s_status.connecting = !live;
    }
    c.epoch = s_epoch;
    s_status.talking = s_status.capture = held;
    s_status.sending = true;
    unlock();
    queue_command(&c);
}

void muse_voice_call_end(void) { atomic_store(&s_stop,true); }

bool muse_voice_call_audio(const int16_t *pcm, size_t frames)
{
    if (!s_lock || frames > PCM_CHUNK || !frames) return false;
    command_t c = {.kind = CMD_AUDIO, .frames = frames};
    lock(); bool take = s_active && s_held && !failed(); c.epoch = s_epoch; unlock();
    if (!take) return false;
    memcpy(c.pcm,pcm,frames * sizeof(*pcm));
    queue_command(&c);
    return !failed();
}

size_t muse_voice_call_read(int16_t *pcm, size_t frames, bool *flushed)
{
    if (!s_lock) { *flushed = false; return 0; }
    lock();
    *flushed = s_flushed; s_flushed = false;
    size_t n = s_held || failed() ? 0 : (frames < s_count ? frames : s_count);
    for (size_t i = 0; i < n; ++i) pcm[i] = s_output[(s_head + i) % OUTPUT_FRAMES];
    s_head = (s_head + n) % OUTPUT_FRAMES; s_count -= n;
    unlock();
    return n;
}

/* Called by the same voice task after a successful speaker write. */
void muse_realtime_bridge_played(size_t frames)
{
    lock(); s_written += frames; s_last_write = esp_timer_get_time(); unlock();
}

muse_voice_call_ev_t muse_voice_call_event(char *text, size_t cap)
{
    if (!s_lock) return MUSE_VOICE_CALL_EV_NONE;
    muse_voice_call_ev_t ev = MUSE_VOICE_CALL_EV_NONE;
    lock();
    if (cap) text[0] = 0;
    if (s_end_event) {
        ev = MUSE_VOICE_CALL_EV_ENDED; s_end_event = false; s_ready_event = s_caption_event = false;
        if (cap) snprintf(text,cap,"%s",atomic_load(&s_failed) ? "BRIDGE DISCONNECTED" : "CALL ENDED");
    } else if (s_ready_event) { ev = MUSE_VOICE_CALL_EV_READY; s_ready_event = false; }
    else if (s_caption_event) {
        ev = MUSE_VOICE_CALL_EV_REPLY; s_caption_event = false;
        if (cap) snprintf(text,cap,"%s",s_caption);
    }
    unlock(); return ev;
}
void muse_voice_call_ptt_status(muse_voice_call_ptt_status_t *out)
{
    if (!s_lock) { memset(out,0,sizeof(*out)); return; }
    lock(); *out = s_status; unlock();
}
bool muse_voice_call_working(void)
{
    if (!s_lock) return false;
    lock(); bool working = s_status.working; unlock(); return working;
}

/* The Muse chat/management Noise session has no ownership of this WSS call. */
bool muse_voice_call_poll(void) { return true; }
bool muse_voice_call_busy(void) { return false; }
bool muse_voice_call_rx_full(void) { return false; }
void muse_voice_call_head(int64_t id,int status,const char *date,size_t len)
{ (void)id; (void)status; (void)date; (void)len; }
void muse_voice_call_data(int64_t id,const uint8_t *data,size_t len)
{ (void)id; (void)data; (void)len; }
void muse_voice_call_stream_end(int64_t id,bool ok,int status)
{ (void)id; (void)ok; (void)status; }
void muse_voice_call_disconnected(void) {}
