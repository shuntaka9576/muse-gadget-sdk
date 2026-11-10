# StopWatch + Realtime bridge (experimental)

This opt-in backend connects the StopWatch to
[muse-realtime-bridge](https://github.com/shuntaka9576/muse-realtime-bridge)
over WSS. The bridge connects to OpenAI. Normal Muse voice remains the default.
The gadget's pairing and management connection still use Muse.

## Connection setup

Run the bridge and provide a device-reachable **WSS** endpoint ending in
`/realtime`. A TLS reverse proxy must forward WebSocket upgrades and the
`Authorization` header. Use a certificate trusted by the ESP-IDF certificate
bundle. The firmware does not disable certificate or hostname verification.
The standard Muse VM API / Noise connection does not automatically expose this
listener: a reachable endpoint must be arranged separately.

In the StopWatch build's `menuconfig`, enable:

- `MUSE_REALTIME_VOICE`
- `MUSE_REALTIME_BRIDGE`
- `MUSE_REALTIME_BRIDGE_URL`: `wss://bridge.example.com/realtime`
- `MUSE_REALTIME_BRIDGE_TOKEN`: the bridge's `BRIDGE_TOKEN` (32–256 printable
  non-space characters).

The token is a **bridge-only credential**, not the Muse VM token, the SDK token,
or an OpenAI key. It is stored in the ignored build `sdkconfig` and compiled
into firmware. Keep both private; rotate the bridge token if a device/image is
lost. The OpenAI credential stays on the VM side; see the bridge's credential
store setup instructions.

Use ESP-IDF **6.0.3** and the existing StopWatch overlays. For example, from
`esp32/`, with the SDK token already set in the ignored build configuration:

```sh
idf.py -B build-muse-m5stack-stopwatch-bridge \
  -D IDF_TARGET=esp32s3 \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch' menuconfig
idf.py -B build-muse-m5stack-stopwatch-bridge build
```

No endpoint/token values are committed. The image builds with empty defaults,
but calls show `SET BRIDGE URL / TOKEN` until configured.

## Operation

Hold the existing talk button to record; release it to request a response.
Bridge mode always uses push-to-talk, regardless of the voice-note setting.
The blue end-call button closes the connection. A later call creates a fresh
session. Connection failure ends the call without replaying microphone audio.

Device audio is mono PCM16 at 16 kHz. The backend resamples to/from the bridge's
24 kHz PCM16, sends 40 ms input frames, and waits for `bridge.ready` before
sending. Holds shorter than 100 ms of captured audio are cleared. Input during
connection setup is buffered for up to six seconds. Queue overflow, invalid
protocol data, connection loss, or the ten-minute session limit ends the call.
Playback buffering is capped at 30 seconds. The bridge also applies its own
input idle and duration limits.

Pressing during a reply drops queued playback, cancels generation, and truncates
the conversation's audio item to a conservative playback position. This position
uses successful speaker writes minus an estimated 120 ms hardware pipeline;
it is **not measured at the DAC**. Audio already in the speaker pipeline can
remain for roughly that interval. Late audio from the interrupted response is
ignored. A new response waits for the previous `response.done`. The backend
supports one audio item (content index 0) per response and ends the call rather
than misattributing audio if that contract changes.

## Validation

The host tests cover protocol negotiation, TLS/token configuration validation,
fragmented WebSocket messages, bounds, and interrupted response ordering:

```sh
python3 -m unittest discover -s tests -p 'test_muse_realtime_bridge.py'
```

Build verification does not verify the VM route, Sentinel credential insertion,
or physical microphone/speaker timing. After configuring a real endpoint, test
first words during connection setup, short taps, normal responses, interruption
before/after audio starts, quick repeated presses, Wi-Fi loss and reconnect,
and the blue end-call button on the device.
