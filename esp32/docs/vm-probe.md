# StopWatch VM echo check

Enable `CONFIG_MUSE_VM_PROBE=y` in the StopWatch build's sdkconfig (ESP-IDF 6.0.3).
This overrides the yellow-button voice action; it does not require a bridge URL or token.
Use the already-paired device, connect Wi-Fi, wake the screen, then press yellow.
The diagnostic runs without initializing I2S or allocating audio buffers. No audio is sent. Disable the option to restore the selected voice backend.

The probe checks VM lookup through the SDK's saved device credentials and refresh handling,
then waits up to 20 seconds for the existing Link Noise session. It does not create another
session or modify device registration. It sends GET /probe/echo?nonce=<32 hex digits>,
waits up to 15 seconds, and accepts only HTTP 200 with matching server and nonce.
Account lookup uses the existing SDK timeout behavior. An input-epoch change or shutdown
cancels after the lookup returns or during the subsequent waits.

Screen/log stages: VM INFO, NOISE, ECHO. HTTP 404 indicates the path did not reach a working
echo route; authentication alone does not provide arbitrary localhost forwarding.
No token or raw response body is printed. Late callbacks are ignored after cancel/retry.
The single VM server remains muse-realtime-bridge on 127.0.0.1:8787.

Host test: `python3 -m unittest discover -s tests -p test_muse_vm_probe.py` from esp32.
