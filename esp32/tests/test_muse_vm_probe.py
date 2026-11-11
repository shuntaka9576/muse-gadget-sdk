"""Run the actual echo worker with fake Link transport, without credentials/network."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProbeTests(unittest.TestCase):
    def test_transport_and_response_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'freertos').mkdir()
            common = '''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(tag,fmt,...) ((void)0)
void vTaskDelay(int);
int64_t esp_timer_get_time(void);
void esp_fill_random(void *, size_t);
void muse_state_set_caption(const char *, ...);
bool muse_link_hatch_linked(void);
bool muse_wifi_connected(void);
bool muse_link_hatch_vm(const char *,char *,size_t,char *,size_t,char **);
bool muse_link_req_ready(void);
typedef void (*muse_link_req_cb)(void *,int,const uint8_t *,size_t,bool);
int64_t muse_link_req_open(const char *,const char *,const char *const *,bool,muse_link_req_cb,void *);
void muse_link_req_cancel(int64_t);
'''
            (p / 'fake.h').write_text(common)
            for name in ['esp_log.h', 'esp_random.h', 'esp_timer.h', 'muse_link.h',
                         'muse_state.h', 'muse_wifi.h', 'freertos/FreeRTOS.h', 'freertos/task.h']:
                (p / name).write_text('#include "fake.h"\n')
            for name in ['muse_vm_probe.c', 'muse_vm_probe.h']:
                (p / name).write_text((ROOT / 'components/muse' / name).read_text())
            (p / 'test.c').write_text(r'''
#include "fake.h"
#include "muse_vm_probe.h"
#include <assert.h>
#include <stdlib.h>
static int mode;
static int64_t now;
static muse_link_req_cb old_cb;
static void *old_ctx;
void vTaskDelay(int n) { now += n * 1000; }
int64_t esp_timer_get_time(void) { return now; }
void esp_fill_random(void *p, size_t n) { memset(p, 0xab, n); }
void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
bool muse_link_hatch_linked(void) { return mode != 1; }
bool muse_wifi_connected(void) { return mode != 2; }
bool muse_link_hatch_vm(const char *a,char *b,size_t c,char *d,size_t e,char **t)
{ (void)a;(void)b;(void)c;(void)d;(void)e; *t = strdup("fake-only"); return mode != 3; }
bool muse_link_req_ready(void) { return mode != 4; }
bool cancelled(void) { return mode == 10; }
void muse_link_req_cancel(int64_t id) { (void)id; }
int64_t muse_link_req_open(const char *verb,const char *path,const char *const *h,bool end,muse_link_req_cb cb,void *ctx)
{
 assert(!strcmp(verb,"GET") && !h && end);
 assert(!strcmp(path,"/probe/echo?nonce=abababababababababababababababab"));
 if (old_cb) old_cb(old_ctx, 401, NULL, 0, true); /* late previous request */
 old_cb=cb; old_ctx=ctx;
 if (mode==5) return 0;
 if (mode==6) cb(ctx,404,NULL,0,true);
 else if (mode==7) { char large[600]={0}; cb(ctx,200,(uint8_t*)large,sizeof(large),true); }
 else if (mode==8) cb(ctx,200,(const uint8_t*)"{}",2,true);
 else if (mode==9) return 1; /* timeout */
 else if (mode==11) cb(ctx,-1,NULL,0,true);
 else {
  const char *body="{\"server\":\"muse-vm-probe\",\"nonce\":\"abababababababababababababababab\"}";
  cb(ctx,200,(const uint8_t*)body,12,false);
  cb(ctx,0,(const uint8_t*)body+12,strlen(body)-12,true);
 }
 return 1;
}
int main(void) {
 const char *expected[]={"ECHO PASS","PAIR WITH MUSE","NO WI-FI","VM INFO: FAILED",
 "NOISE FAILED","ECHO OPEN FAILED","HTTP 404","RESPONSE TOO LARGE","RESPONSE MISMATCH",
 "ECHO TIMEOUT","CANCELLED","TRANSPORT FAILED"};
 for(mode=0;mode<12;mode++) { char result[96]; now=0; muse_vm_probe_run(result,sizeof(result),cancelled); assert(strstr(result,expected[mode])); }
 mode=0; char result[96]; muse_vm_probe_run(result,sizeof(result),cancelled); assert(strstr(result,"ECHO PASS"));
}
''')
            cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
            if not cjson.exists():
                cjson = next((ROOT / 'managed_components').glob('**/cJSON.c')).parent
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DARWIN_C_SOURCE',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            '-I', str(p), '-I', str(cjson), str(p / 'test.c'),
                            str(p / 'muse_vm_probe.c'), str(cjson / 'cJSON.c'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
