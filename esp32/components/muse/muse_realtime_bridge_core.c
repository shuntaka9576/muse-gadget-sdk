#include "muse_realtime_bridge_core.h"
#include <string.h>

bool muse_rb_config_valid(const char *url, const char *token)
{
    if (!url || !token || strncmp(url, "wss://", 6) || strlen(url) > 512) return false;
    const char *path = strchr(url + 6, '/');
    if (!path || path == url + 6 || strcmp(path, "/realtime")) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p >= 127 || strchr("@?#\\", *p)) return false;
    size_t n = strlen(token);
    if (n < 32 || n > 256) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) return false;
    return true;
}

bool muse_rb_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    muse_vc_span_t s;
    /* Reject long encoded values instead of silently truncating identities. */
    if (muse_vc_json_get(json, len, key, &s) != MUSE_VC_J_STRING || s.n >= cap) return false;
    if (!muse_vc_json_unescape(s, out, cap)) return false;
    /* IDs/types are ASCII in this protocol; embedded NUL must not alias IDs. */
    for (size_t i = 0; i + 5 < s.n; ++i)
        if (!memcmp(s.p + i, "\\u0000", 6)) return false;
    return true;
}

static bool number(const char *j, size_t n, const char *key, const char *value)
{
    muse_vc_span_t s;
    return muse_vc_json_get(j, n, key, &s) == MUSE_VC_J_NUMBER &&
           s.n == strlen(value) && !memcmp(s.p, value, s.n);
}

bool muse_rb_ready(const char *j, size_t n)
{
    char type[40], enc[32];
    return muse_rb_string(j,n,"type",type,sizeof(type)) && !strcmp(type,"bridge.ready") &&
           muse_rb_string(j,n,"audio.encoding",enc,sizeof(enc)) && !strcmp(enc,"pcm_s16le") &&
           number(j,n,"protocol_version","1") && number(j,n,"audio.sample_rate","24000") &&
           number(j,n,"audio.channels","1");
}

int muse_rb_rx_feed(muse_rb_rx_t *r, char *buf, size_t cap, int opcode, bool fin,
                    size_t total, size_t offset, const char *data, size_t n)
{
    if (opcode >= 8) return 0; /* ping/pong/close do not disturb fragmented text */
    if (!cap || offset > total || n > total - offset) return -1;
    if (!offset) {
        if (r->frame_open) return -1;
        if (opcode == 1) {
            if (r->message_open) return -1;
            r->used = 0;
            r->message_open = true;
        } else if (opcode != 0 || !r->message_open) return -1;
        r->frame_open = true;
        r->frame_offset = 0;
        r->frame_size = total;
    }
    if (!r->frame_open || total != r->frame_size || offset != r->frame_offset ||
        r->used >= cap || n > cap - 1 - r->used) return -1;
    if (n) memcpy(buf + r->used, data, n);
    r->used += n;
    r->frame_offset += n;
    if (r->frame_offset != total) return 0;
    r->frame_open = false;
    if (!fin) return 0;
    r->message_open = false;
    buf[r->used] = 0;
    return 1;
}

void muse_rb_turn_press(muse_rb_turn_t *t) { t->create_queued = false; }
bool muse_rb_turn_create(muse_rb_turn_t *t, unsigned epoch)
{
    if (t->pending || !t->create_queued) return false;
    t->pending = true;
    t->create_queued = false;
    t->requested_epoch = epoch;
    t->response[0] = 0;
    return true;
}
bool muse_rb_turn_created(muse_rb_turn_t *t, const char *id)
{
    if (!t->pending || t->response[0] || !id[0] || strlen(id) >= sizeof(t->response)) return false;
    strcpy(t->response, id);
    return true;
}
bool muse_rb_turn_done(muse_rb_turn_t *t, const char *id)
{
    if (!t->pending || !t->response[0] || strcmp(id, t->response)) return false;
    t->pending = false;
    /* Keep the id: output already queued can still play after response.done. */
    return true;
}
bool muse_rb_turn_audio(const muse_rb_turn_t *t, unsigned epoch, const char *id)
{
    return t->requested_epoch == epoch && t->response[0] && !strcmp(id, t->response);
}
