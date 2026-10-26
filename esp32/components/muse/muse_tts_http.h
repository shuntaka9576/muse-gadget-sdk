#pragma once
#include <stddef.h>
#include <stdint.h>

struct muse_tts_result_t {
    uint32_t generation;
    int message;
    uint8_t *audio;  // caller frees; null on failure
    size_t length;
};
// Called only from the chat task. The worker never touches chat state.
bool muse_tts_http_start(const char *text, uint32_t generation, int message, size_t capacity);
bool muse_tts_http_poll(muse_tts_result_t *result);
void muse_tts_http_cancel(void);
