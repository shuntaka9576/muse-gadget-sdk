"""Protocol v1 boundaries and interruption races, with production C helpers."""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'

class Rx(C.Structure):
    _fields_ = [('used', C.c_size_t), ('frame_size', C.c_size_t),
                ('frame_offset', C.c_size_t), ('message_open', C.c_bool), ('frame_open', C.c_bool)]

class Turn(C.Structure):
    _fields_ = [('epoch', C.c_uint), ('pending', C.c_bool),
                ('queued', C.c_bool), ('response', C.c_char * 129)]

class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'bridge.so'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I', str(MUSE),
                        str(MUSE / 'muse_realtime_bridge_core.c'),
                        str(MUSE / 'muse_voice_call_core.c'), '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.muse_rb_config_valid.argtypes = [C.c_char_p, C.c_char_p]
        cls.lib.muse_rb_config_valid.restype = C.c_bool
        cls.lib.muse_rb_ready.argtypes = [C.c_char_p, C.c_size_t]
        cls.lib.muse_rb_ready.restype = C.c_bool
        cls.lib.muse_rb_string.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_char_p, C.c_size_t]
        cls.lib.muse_rb_string.restype = C.c_bool
        cls.lib.muse_rb_rx_feed.argtypes = [C.POINTER(Rx), C.c_char_p, C.c_size_t,
                                          C.c_int, C.c_bool, C.c_size_t, C.c_size_t, C.c_char_p, C.c_size_t]
        for name, arg in [('create', C.c_uint), ('created', C.c_char_p), ('done', C.c_char_p)]:
            fn = getattr(cls.lib, 'muse_rb_turn_' + name)
            fn.argtypes = [C.POINTER(Turn), arg]
            fn.restype = C.c_bool
        cls.lib.muse_rb_turn_audio.argtypes = [C.POINTER(Turn), C.c_uint, C.c_char_p]
        cls.lib.muse_rb_turn_audio.restype = C.c_bool
        cls.lib.muse_rb_turn_press.argtypes = [C.POINTER(Turn)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_credentials_and_tls(self):
        valid = self.lib.muse_rb_config_valid
        self.assertTrue(valid(b'wss://bridge.example.com/realtime', b'x' * 32))
        self.assertTrue(valid(b'wss://bridge.example.com:8443/realtime', b'x' * 256))
        for url in [b'ws://host/realtime', b'wss:///realtime', b'wss://user:pw@host/realtime',
                    b'wss://host/realtime?token=x', b'wss://host/realtime#x', b'wss://host/other',
                    b'wss://host\r\n/realtime']:
            self.assertFalse(valid(url, b'x' * 32), url)
        for token in [b'x' * 31, b'x' * 257, b'x' * 32 + b'\r\nHeader: x', b'x' * 32 + b' ']:
            self.assertFalse(valid(b'wss://host/realtime', token))

    def test_ready_audio_contract(self):
        event = {'type': 'bridge.ready', 'protocol_version': 1,
                 'audio': {'encoding': 'pcm_s16le', 'sample_rate': 24000, 'channels': 1}}
        def ready(value):
            b = json.dumps(value).encode()
            return self.lib.muse_rb_ready(b, len(b))
        self.assertTrue(ready(event))
        for key, value in [('encoding', 'opus'), ('sample_rate', 16000), ('channels', 2)]:
            bad = {**event, 'audio': {**event['audio'], key: value}}
            self.assertFalse(ready(bad))
        self.assertFalse(ready({**event, 'protocol_version': 2}))

    def feed(self, r, buf, opcode, fin, total, offset, data):
        return self.lib.muse_rb_rx_feed(C.byref(r), buf, len(buf), opcode, fin,
                                        total, offset, data, len(data))

    def test_fragmented_message_with_split_frames_and_ping(self):
        r, buf = Rx(), C.create_string_buffer(64)
        self.assertEqual(self.feed(r, buf, 1, False, 6, 0, b'{"t'), 0)
        self.assertEqual(self.feed(r, buf, 1, False, 6, 3, b'ype'), 0)
        self.assertEqual(self.feed(r, buf, 9, True, 1, 0, b'?'), 0)
        self.assertEqual(self.feed(r, buf, 0, True, 6, 0, b'":"'), 0)
        self.assertEqual(self.feed(r, buf, 0, True, 6, 3, b'x"}'), 1)
        self.assertEqual(buf.value, b'{"type":"x"}')
        self.assertEqual(self.feed(r, buf, 1, True, 2, 0, b'{}'), 1)
        self.assertEqual(buf.value, b'{}')

    def test_invalid_frame_order_and_bounds(self):
        for args in [(0, True, 1, 0, b'x'), (2, True, 1, 0, b'x'),
                     (1, True, 1, 1, b'x'), (1, True, 8, 0, b'12345678')]:
            self.assertEqual(self.feed(Rx(), C.create_string_buffer(8), *args), -1)
        r, buf = Rx(), C.create_string_buffer(16)
        self.assertEqual(self.feed(r, buf, 1, False, 2, 0, b'a'), 0)
        self.assertEqual(self.feed(r, buf, 1, False, 2, 0, b'b'), -1)

    def test_delayed_response_created_after_interrupt(self):
        t = Turn(queued=True)
        self.assertTrue(self.lib.muse_rb_turn_create(C.byref(t), 1))
        self.lib.muse_rb_turn_press(C.byref(t))
        self.assertTrue(self.lib.muse_rb_turn_created(C.byref(t), b'old'))
        self.assertFalse(self.lib.muse_rb_turn_audio(C.byref(t), 2, b'old'))
        t.queued = True  # next hold committed while cancellation is in flight
        self.assertFalse(self.lib.muse_rb_turn_create(C.byref(t), 2))
        self.assertFalse(self.lib.muse_rb_turn_done(C.byref(t), b'unknown'))
        self.assertTrue(self.lib.muse_rb_turn_done(C.byref(t), b'old'))
        self.assertTrue(self.lib.muse_rb_turn_create(C.byref(t), 2))
        self.assertTrue(self.lib.muse_rb_turn_created(C.byref(t), b'new'))
        self.assertTrue(self.lib.muse_rb_turn_audio(C.byref(t), 2, b'new'))
        self.assertFalse(self.lib.muse_rb_turn_audio(C.byref(t), 2, b'old'))
        self.assertFalse(self.lib.muse_rb_turn_done(C.byref(t), b'old'))

    def test_press_discards_queued_response_and_ids_do_not_truncate(self):
        t = Turn(queued=True)
        self.lib.muse_rb_turn_press(C.byref(t))
        self.assertFalse(self.lib.muse_rb_turn_create(C.byref(t), 1))
        t.queued = True
        self.assertTrue(self.lib.muse_rb_turn_create(C.byref(t), 1))
        self.assertFalse(self.lib.muse_rb_turn_created(C.byref(t), b'x' * 129))
        self.assertFalse(self.lib.muse_rb_turn_created(C.byref(t), b''))
        for value in ['x' * 129, 'foo\x00bar']:
            j = json.dumps({'id': value}).encode()
            out = C.create_string_buffer(129)
            self.assertFalse(self.lib.muse_rb_string(j,len(j),b'id',out,len(out)))

if __name__ == '__main__':
    unittest.main()
