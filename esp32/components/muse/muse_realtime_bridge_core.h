#pragma once

#include "muse_voice_call_core.h"

#define MUSE_RB_MESSAGE_MAX (64 * 1024)
#define MUSE_RB_ID_MAX 129

/* Small protocol helpers shared with the host tests. No platform dependencies. */
bool muse_rb_config_valid(const char *url, const char *token);
bool muse_rb_string(const char *json, size_t len, const char *key, char *out, size_t cap);
bool muse_rb_ready(const char *json, size_t len);

typedef struct {
    size_t used, frame_size, frame_offset;
    bool message_open, frame_open;
} muse_rb_rx_t;
/* Returns -1 for invalid/oversize input, 0 for an incomplete message or a
 * control frame, 1 for a complete text message. Handles both WS fragmentation
 * and the client's splitting a frame across DATA callbacks. */
int muse_rb_rx_feed(muse_rb_rx_t *r, char *buf, size_t cap, int opcode, bool fin,
                    size_t payload_size, size_t offset, const char *data, size_t len);

typedef struct {
    unsigned requested_epoch;
    bool pending, create_queued;
    char response[MUSE_RB_ID_MAX];
} muse_rb_turn_t;
void muse_rb_turn_press(muse_rb_turn_t *t);
bool muse_rb_turn_create(muse_rb_turn_t *t, unsigned epoch);
bool muse_rb_turn_created(muse_rb_turn_t *t, const char *id);
bool muse_rb_turn_done(muse_rb_turn_t *t, const char *id);
bool muse_rb_turn_audio(const muse_rb_turn_t *t, unsigned epoch, const char *id);
