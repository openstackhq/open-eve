# Open EvE backend

Cloudflare Worker (Hono) that powers the Open EvE desk robot. It exposes:

- `POST /transcribe` – proxy to [Sarvam Saaras v3 STT](https://docs.sarvam.ai/api-reference-docs/api-guides-tutorials/speech-to-text/rest-api).
- `POST /chat` – Vercel AI SDK agent (OpenAI `gpt-4.1-mini` + a Firecrawl
  `webSearch` tool) that takes the user's transcript, keeps the last 6 turns
  per device in Cloudflare KV, then synthesises the spoken reply with
  [Sarvam Bulbul v3 TTS](https://docs.sarvam.ai/api-reference-docs/api-guides-tutorials/text-to-speech/rest-api)
  and streams the resulting WAV back to the robot.

The robot ESP32 plays that WAV through a Bluetooth speaker via A2DP source
(see [`src/speak/`](../speak/)).

## Run locally

```sh
bun install
bun run dev      # http://localhost:8787
```

Secrets are read from `.dev.vars` for local dev. For production each one is
stored as a Worker secret:

```sh
bunx wrangler secret put SARVAM_API_KEY
bunx wrangler secret put OPENAI_API_KEY
bunx wrangler secret put FIRECRAWL_API_KEY
bun run deploy
```

The KV namespace `SESSIONS` (used by `/chat` for conversation history) is
created once with:

```sh
bunx wrangler kv namespace create SESSIONS
bunx wrangler kv namespace create SESSIONS --preview
# Paste the returned ids into wrangler.jsonc (already done in this repo).
```

After changing `wrangler.jsonc` or `.dev.vars`, regenerate types:

```sh
bun run cf-typegen
```

## API

### `GET /`

Returns a JSON index of available endpoints.

### `GET /health`

`{ "status": "ok", "timestamp": <ms> }`

### `POST /transcribe`

Forwards audio to Sarvam Saaras v3 and returns the upstream response verbatim:

```json
{
  "request_id": "20241115_...",
  "transcript": "Hello, how are you?",
  "language_code": "en-IN"
}
```

#### Query parameters

| Param           | Default       | Notes                                                               |
| --------------- | ------------- | ------------------------------------------------------------------- |
| `mode`          | `transcribe`  | `transcribe` \| `translate` \| `verbatim` \| `translit` \| `codemix` |
| `language_code` | _(auto)_      | BCP-47 hint, e.g. `hi-IN`, `en-IN`. Most useful for `transcribe`.   |
| `sample_rate`   | `16000`       | Only when sending raw PCM.                                          |
| `channels`      | `1`           | Only when sending raw PCM.                                          |
| `bits_per_sample` | `16`        | Only when sending raw PCM (8/16/24/32).                             |

#### Accepted request bodies

The Worker auto-detects the body shape from `Content-Type`:

1. **`multipart/form-data`** — standard upload with a `file` field. Use any
   format Sarvam accepts (WAV, MP3, AAC, FLAC, OGG).
2. **A binary audio file** — set `Content-Type` to `audio/wav`, `audio/mpeg`,
   `audio/aac`, `audio/flac`, or `audio/ogg` and send the file as the body.
3. **Raw little-endian PCM** — set `Content-Type` to `audio/pcm`,
   `audio/L16`, or `application/octet-stream`. The Worker wraps the samples
   in a WAV header before forwarding. This is the easiest path for an ESP32
   streaming directly from I2S.

#### cURL examples

```sh
# multipart upload
curl -X POST 'http://localhost:8787/transcribe?language_code=en-IN' \
  -F 'file=@recording.wav;type=audio/wav'

# raw WAV body
curl -X POST 'http://localhost:8787/transcribe?language_code=hi-IN&mode=translate' \
  -H 'Content-Type: audio/wav' \
  --data-binary @recording.wav

# raw PCM (16 kHz, 16-bit, mono) — what the ESP32 sends
curl -X POST 'http://localhost:8787/transcribe?sample_rate=16000&channels=1&bits_per_sample=16' \
  -H 'Content-Type: audio/pcm' \
  --data-binary @recording.pcm
```

### `POST /chat`

Runs the AI agent and returns Sarvam TTS audio (WAV, 24 kHz mono, Linear16)
ready for the ESP32 to upsample and stream over A2DP.

#### Request body (JSON)

| Field           | Required | Default   | Notes                                                                 |
| --------------- | -------- | --------- | --------------------------------------------------------------------- |
| `deviceId`      | yes      | –         | Stable id per robot (use the MAC). Used as the KV history key.        |
| `text`          | yes      | –         | Merged user transcript for this turn (1–2000 chars).                  |
| `language_code` | no       | `en-IN`   | Sarvam BCP-47 hint. Steers the TTS voice; the LLM mirrors the user.   |
| `speaker`       | no       | `shubh`   | Any Bulbul v3 speaker (e.g. `shubh`, `priya`, `roopa`).               |
| `reset`         | no       | `false`   | Clear the device's KV history before this turn.                       |

#### Response

`200 audio/wav` body. The text the model said is mirrored back in the
`X-Reply-Text` response header (URL-encoded, max 256 chars). The
`X-History-Len` header returns the new message count after this turn.

The Worker:

1. Loads the last ≤12 messages (6 turns) from KV (`hist:<deviceId>`).
2. Calls `generateText` with the `webSearch` tool. The tool is invoked only
   when the LLM decides; the loop is hard-capped at 3 steps (`stopWhen:
   stepCountIs(3)`).
3. Trims the reply to 1800 characters (Bulbul v3 hard limit is 2500).
4. Writes the updated history back to KV (TTL 30 minutes).
5. POSTs the reply to Sarvam Bulbul v3 (`speech_sample_rate: 24000`),
   base64-decodes the returned WAV, and streams it as the response body.

#### Curl examples

```sh
# Simple turn (no history yet)
curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","text":"Tell me a one-line joke about robots."}' \
  -o reply.wav -D headers.txt
afplay reply.wav   # macOS

# Web-search turn — the agent invokes the Firecrawl tool internally
curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","text":"What is the bitcoin price right now?"}' \
  -o reply.wav

# Memory turn — references the previous question
curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","text":"And in INR?"}' \
  -o reply.wav

# Wipe history before a new conversation
curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","text":"Hi","reset":true}' \
  -o reply.wav
```

## ESP32 client (Arduino sketch)

The robot typically uses an INMP441/ICS-43434 I2S MEMS mic at 16 kHz/16-bit
mono. Buffer one utterance (≤ 30 s — Sarvam sync limit), then POST the raw
PCM bytes:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID  = "...";
const char* WIFI_PASS  = "...";
// Your deployed Worker URL, e.g. https://backend.<account>.workers.dev
const char* STT_URL    = "https://backend.example.workers.dev/transcribe"
                         "?sample_rate=16000&channels=1&bits_per_sample=16"
                         "&language_code=en-IN";

// pcm_buf / pcm_len is your I2S capture buffer (signed 16-bit LE samples).
String transcribe(const uint8_t* pcm_buf, size_t pcm_len) {
  HTTPClient http;
  http.begin(STT_URL);
  http.addHeader("Content-Type", "audio/pcm");
  http.setTimeout(20000);

  int status = http.POST(const_cast<uint8_t*>(pcm_buf), pcm_len);
  String body = http.getString();
  http.end();

  Serial.printf("STT %d: %s\n", status, body.c_str());
  return body; // JSON: { request_id, transcript, language_code }
}
```

Tips for the firmware:

- Record into PSRAM if available; 30 s @ 16 kHz/16-bit mono = ~960 KB.
- Trigger via push-to-talk or simple energy-based VAD to keep clips short.
- Always `WiFi.setSleep(false)` before a large POST and confirm the Worker
  URL is HTTPS; ESP32 needs the root CA chain (use `WiFiClientSecure` with
  `setInsecure()` for prototyping, but switch to a real CA bundle for prod).
- Parse the response JSON with ArduinoJson and read `transcript`.

## Project layout

```
src/backend/
├── src/index.ts            # Hono app: /transcribe + /chat agent + /health
├── wrangler.jsonc          # Worker config: KV binding, nodejs_compat, observability
├── worker-configuration.d.ts  # generated by `bun run cf-typegen`
├── .dev.vars               # local SARVAM_API_KEY / OPENAI_API_KEY / FIRECRAWL_API_KEY (gitignored)
└── package.json            # ai, @ai-sdk/openai, @mendable/firecrawl-js, hono, zod
```

## Pipeline (full request)

```
ESP32 mic --(VAD)--> POST /transcribe (audio/pcm) --> Sarvam STT --> transcript
                                                          |
                            client-side merge (1.5 s)     |
                                                          v
ESP32 (BT speaker) <-- audio/wav <-- POST /chat (deviceId, text)
                                          |
                                          +-- KV: load last 6 turns
                                          +-- AI SDK generateText (gpt-4.1-mini)
                                          |     +-- tool: webSearch -> Firecrawl /search
                                          +-- KV: save updated history (TTL 30 min)
                                          +-- Sarvam Bulbul v3 TTS (24 kHz mono WAV)
```
