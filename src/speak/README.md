# Open EvE - voice agent firmware (`speech-to-text.ino`)

Single-board ESP32 sketch that runs the full voice loop:

1. INMP441 I2S mic + adaptive energy VAD.
2. POST raw PCM to the Cloudflare Worker `/transcribe` endpoint -> Sarvam STT.
3. Client-side merge: transcripts that arrive within 1500 ms of each other
   are concatenated into one user turn (hard cap: 6 fragments).
4. POST the merged text to `/chat` -> the Worker runs the Vercel AI SDK
   agent with a Firecrawl `webSearch` tool, then synthesises the spoken
   reply with Sarvam Bulbul v3 TTS.
5. The WAV body (24 kHz mono Linear16) is streamed as it arrives, upsampled
   on the fly to 44.1 kHz stereo, and pushed into a FreeRTOS StreamBuffer.
6. The pschatzmann ESP32-A2DP source library SBC-encodes the StreamBuffer
   into a Bluetooth speaker.

Everything runs concurrently across both ESP32 cores; the I2S mic keeps
listening even while a reply is playing.

## Hardware

| Part                          | Notes                                                  |
| ----------------------------- | ------------------------------------------------------ |
| ESP32 dev board (classic)     | **must** be classic ESP32 (WROOM/WROVER). ESP32-S3 has no A2DP. |
| ESP32-WROVER (PSRAM)          | strongly recommended; lets you raise `MAX_UTTERANCE_SECONDS`. |
| INMP441 I2S mic               | wired WS=GPIO25, SD=GPIO33, SCK=GPIO26.                |
| Bluetooth speaker             | any A2DP sink. Set `BT_SPEAKER_NAME` to its exact name.|
| 5 V USB power supply          | the BT stack + Wi-Fi + I2S together can pull >300 mA. |

## Toolchain

This is an Arduino sketch. Build with either:

### Option A: PlatformIO (recommended)

A [`platformio.ini`](../../platformio.ini) at the repo root already wires
this sketch to the `esp32-voice` environment:

- `board = esp32dev` (classic ESP32 — required for A2DP).
- `board_build.partitions = huge_app.csv` (Bluetooth Classic + Wi-Fi + TLS
  doesn't fit the default 1.2 MB app slot).
- `lib_deps = pschatzmann/ESP32-A2DP@^1.8.10`.
- `build_src_filter` keeps only `src/speak/` so the eyes sketch and the
  Cloudflare Worker source aren't compiled into the firmware.

```sh
pio run -e esp32-voice                # build
pio run -e esp32-voice -t upload      # flash
pio device monitor -e esp32-voice -b 115200   # serial monitor
```

A vendored copy of the ESP32-A2DP library also lives at
[`lib/ESP32-A2DP/`](../../lib/ESP32-A2DP/) for offline builds (PlatformIO's
default `lib_dir` is `lib/`, so it's auto-discovered).

### Option B: Arduino IDE

Install via the Library Manager:

| Library                                                      | Why                  |
| ------------------------------------------------------------ | -------------------- |
| [pschatzmann/ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) | `BluetoothA2DPSource`|

Everything else (`WiFi`, `WiFiClientSecure`, `HTTPClient`, FreeRTOS, the
legacy `driver/i2s.h`) ships with the ESP32 Arduino core (>=3.0 recommended).

Board settings:

- *Tools -> Board*: any classic ESP32 (e.g. *ESP32 Dev Module*). **Not** an S3.
- *Tools -> Partition Scheme*: *Huge APP (3 MB No OTA / 1 MB SPIFFS)* —
  required for the Bluetooth Classic stack to fit.
- *Tools -> PSRAM*: Enabled (only if you have a WROVER). The sketch's
  allocator (`tryAllocBytes`) tries PSRAM first then falls back.

### IntelliSense (VS Code)

If the C/C++ extension reports `cannot open source file
"BluetoothA2DPSource.h"`, it just means it can't find the cloned library on
disk. The repo ships
[`.vscode/c_cpp_properties.json`](../../.vscode/c_cpp_properties.json)
already pointing at both `lib/ESP32-A2DP/src` and
`.pio/libdeps/esp32-voice/ESP32-A2DP/src`, so either of these resolves it:

```sh
# Vendored copy already exists - just reload the C/C++ extension.
ls lib/ESP32-A2DP/src/BluetoothA2DPSource.h

# Or let PlatformIO install into .pio/libdeps/...
pio pkg install -e esp32-voice
```

Then run *C/C++: Reset IntelliSense Database* in VS Code.

## Configure

Open [`speech-to-text.ino`](./speech-to-text.ino) and edit the top constants:

| Constant            | What                                                         |
| ------------------- | ------------------------------------------------------------ |
| `WIFI_SSID` / `WIFI_PASS` | Your 2.4 GHz Wi-Fi (the ESP32 cannot use 5 GHz).         |
| `BACKEND_HOST`      | Your Cloudflare Worker URL (`https://...workers.dev`).        |
| `BT_SPEAKER_NAME`   | Exact advertised name of your Bluetooth speaker.              |
| `MAX_UTTERANCE_SECONDS` | 2 (default) – raise to 3-4 only with PSRAM.               |
| `MIC_SHIFT`         | 11 (default). Lower = louder. See header comments.           |

The backend (`src/backend/`) must be deployed and reachable, and its three
secrets (`SARVAM_API_KEY`, `OPENAI_API_KEY`, `FIRECRAWL_API_KEY`) must be
set. See [`../backend/README.md`](../backend/README.md).

## Build & flash

```sh
# PlatformIO (from repo root):
pio run -e esp32-voice -t upload && pio device monitor -e esp32-voice -b 115200

# Arduino IDE: open speech-to-text.ino, select your classic ESP32 board, the
# Huge APP partition scheme, the right serial port, then click Upload.
```

## Verification (hardware-in-the-loop)

These are the runtime checks the firmware was designed to pass. Run through
them after the first flash; tune buffers if anything trips.

1. **Boot log**

   ```
   Heap at boot: free=…, largest=…, psram_free=…
   utterance pool: 2 x 65536 bytes (2 s each). free=…
   WiFi connected, IP: 192.168.…
   Mic ready
   A2DP source started, scanning for 'JBL Flip 5'...
   Voice agent ready. frame=20ms preroll=300ms hangover=700ms ...
   ```

   The free heap after boot should be > 60 KB. If it's lower, drop
   `MAX_UTTERANCE_SECONDS` to 1 or move to a WROVER.

2. **Bluetooth pairing**

   Power on the speaker. Within ~10 s the speaker should beep / connect.
   Once paired, A2DP auto-reconnects on subsequent boots
   (`set_auto_reconnect(true)`).

3. **STT round-trip**

   Speak normally for ~1 s. Expect:

   ```
   [trigger] rms=… peak=… noise=… snr=… slot=0
   [utt] end of speech. voiced=…ms total=…ms -> queue slot 0
   [net] POST /transcribe …
   [net] transcript (… ms): hello there
   ```

   STT should round-trip in ~700-1500 ms.

4. **Merge window**

   Say "what is the weather", pause briefly, then "in Delhi". Both
   transcripts should be glued before flushing:

   ```
   [merge] +fragment (1 total): what is the weather
   [merge] +fragment (2 total): what is the weather in Delhi
   [merge] 1500 ms idle -> flush: what is the weather in Delhi
   ```

5. **Agent + TTS playback**

   ```
   [chat] POST /chat (NN text bytes)
   [chat] reply preview (urlenc): The%20weather%20in%20Delhi…
   [chat] done: 480000 PCM bytes (cl=480044, remain=0), total 4500 ms, first audio +1500 ms
   ```

   Audio should start coming out of the speaker within ~1.5 s of the POST
   and play to completion without dropouts. If you hear scratches, raise
   `AUDIO_RING_BYTES` in the sketch (e.g. 32 KB) - this absorbs longer
   Wi-Fi/BT coexistence gaps.

6. **Memory under load**

   While the reply is playing, start speaking again. The mic should still
   trigger and capture (vad_task is on a different core from BT). Watch
   the periodic `[idle] heap=…` log - free heap during playback should
   stay above ~50 KB.

## Tuning cheat sheet

| Symptom                                  | Likely fix                                                              |
| ---------------------------------------- | ----------------------------------------------------------------------- |
| Random "buffer full" finalisations       | Raise `MAX_UTTERANCE_SECONDS` (PSRAM) or speak in shorter bursts.       |
| Robot triggers on background hum         | Raise `ABSOLUTE_MIN_PEAK` (e.g. 2000) and/or `TRIGGER_SNR` (e.g. 3.5).   |
| Robot misses your voice across the room  | Lower `ABSOLUTE_MIN_PEAK` (e.g. 800) and `MIC_SHIFT` (e.g. 9).          |
| Audio scratchy / drops out               | Raise `AUDIO_RING_BYTES` to 32-64 KB. Confirm `WiFi.setSleep(false)`.   |
| Reply takes forever to start             | Use a closer Bluetooth speaker; check the Worker logs for slow agent.   |
| Reply ends a few words early             | Increase `stream_deadline` in `postChatAndStream` (currently 30 s).     |

## Project layout

```
open-eve/
├── platformio.ini          # esp32-voice (this sketch) + esp32-eyes envs
├── lib/ESP32-A2DP/         # vendored A2DP library (gitignored, reproducible from lib_deps)
└── src/speak/
    ├── speech-to-text.ino  # full firmware (mic + STT + merge + chat + A2DP)
    └── README.md           # this file
```
