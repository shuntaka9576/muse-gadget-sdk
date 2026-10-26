"""Host tests of the production HTTPS response guards and playback handoff."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class HttpTts(unittest.TestCase):
    def test_guards_and_playback(self):
        http = (ROOT / 'components/muse/muse_tts_http.cpp').read_text()
        chat = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        guard = http[http.index('struct download_t'):http.index('static void worker')]
        playback = chat[chat.index('static void show_tts_as_text'):chat.index('static void tts_data')]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "muse_tts_http.h"
#define CONFIG_MUSE_HTTP_TTS 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MIC_RATE 16000
#define TEXT_CHARS_PER_S 16
#define TEXT_MAX 1024
#define MP3_BUF 128
using esp_err_t = int;
const int ESP_OK = 0, ESP_FAIL = -1, HTTP_EVENT_ON_HEADER = 1, HTTP_EVENT_ON_DATA = 2;
struct esp_http_client_event_t { void *user_data; int event_id; const char *header_key; const char *header_value; int data_len; void *client; const char *data; };
static std::atomic<uint32_t> s_ticket{1};
static int64_t clock_us;
static int status = 200;
static int64_t esp_timer_get_time() { return clock_us; }
static int esp_http_client_get_status_code(void *) { return status; }
''' + guard + r'''
enum { P_IDLE, P_WAIT_REPLY, TTS_QUEUED, TTS_ACTIVE, M_MP3, M_TTS };
struct msg_t { unsigned pcm_start, pcm_frames, len; int tts; };
struct {
 int phase, tts_msg, nmsgs, kbps, down_rate, dec; uint32_t gen, pcm_out;
 bool silent, mp3_ended; uint8_t mp3[MP3_BUF]; size_t mp3_len; char *texts; msg_t msgs[2];
} s_turn;
static std::atomic<uint32_t> s_gen{7};
static bool speaker = true, accept = true, pending;
static int requests;
static muse_tts_result_t result;
static bool muse_settings_speaker_on() { return speaker; }
static int muse_settings_volume() { return 80; }
static void mark(int) {}
static void show_reply_start(const msg_t &) {}
static void mp3dec_init(int *) {}
bool muse_tts_http_start(const char *, uint32_t, int, size_t) { ++requests; return accept; }
bool muse_tts_http_poll(muse_tts_result_t *r) { if (!pending) return false; *r = result; pending = false; return true; }
''' + playback + r'''
int main() {
 uint8_t buf[4] = {};
 download_t d{buf, 0, sizeof(buf), 1, 100, false, false};
 esp_http_client_event_t e{&d, HTTP_EVENT_ON_HEADER, "Content-Type", "audio/mpeg", 0, nullptr, "abcd"};
 assert(on_http(&e) == ESP_OK && d.audio_type);
 e.event_id = HTTP_EVENT_ON_DATA; e.data_len = 4;
 assert(on_http(&e) == ESP_OK && d.length == 4 && !memcmp(buf, "abcd", 4));
 e.data_len = 1;
 assert(on_http(&e) == ESP_FAIL && d.length == 4);
 d.length = 0; d.failed = false; d.audio_type = false;
 assert(on_http(&e) == ESP_FAIL && !d.length);
 d.audio_type = true; status = 403;
 assert(on_http(&e) == ESP_FAIL && !d.length);
 status = 302; assert(on_http(&e) == ESP_FAIL && !d.length);
 status = 200; s_ticket = 2;
 assert(on_http(&e) == ESP_FAIL && !d.length);
 s_ticket = 1; clock_us = 101;
 assert(on_http(&e) == ESP_FAIL && !d.length);
 char text[] = "reply";
 s_turn.phase = P_WAIT_REPLY; s_turn.gen = 7; s_turn.tts_msg = -1;
 s_turn.nmsgs = 1; s_turn.texts = text; s_turn.msgs[0].tts = TTS_QUEUED;
 start_tts(); assert(requests == 1 && !s_turn.silent && s_turn.tts_msg == 0);
 start_tts(); assert(requests == 1);
 auto complete = [](uint32_t gen, bool ok) {
   result = {gen, 0, ok ? static_cast<uint8_t *>(malloc(4)) : nullptr, ok ? 4u : 0u};
   if (result.audio) memcpy(result.audio, "mp3!", 4);
   pending = true; poll_http_tts();
 };
 complete(6, true); assert(!s_turn.mp3_len);
 complete(7, true); assert(s_turn.mp3_len == 4 && s_turn.mp3_ended);
 complete(7, false); assert(s_turn.silent && !s_turn.mp3_len);
 s_turn.tts_msg = -1; s_turn.msgs[0].tts = TTS_QUEUED; speaker = false;
 start_tts(); assert(requests == 1 && s_turn.silent);
 speaker = true; s_turn.tts_msg = -1; s_turn.msgs[0].tts = TTS_QUEUED; accept = false;
 start_tts(); assert(s_turn.silent);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            source.write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'components/muse'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
