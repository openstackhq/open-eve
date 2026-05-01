// Open EvE - ESP32 voice agent (mic + Wi-Fi + Bluetooth A2DP source).
//
// Architecture (FreeRTOS, dual-core classic ESP32 / ESP32-WROOM/WROVER):
//
//   Core 0:
//     vad_task        - I2S capture + adaptive energy VAD. Hands a finalized
//                       utterance buffer to net_task via utt_queue.
//     bt source task  - Owned by the ESP32-A2DP library. Pulls 44.1 kHz
//                       stereo PCM out of the audio_ring StreamBuffer through
//                       a2dp_data_cb() and SBC-encodes it for the speaker.
//   Core 1:
//     net_task        - Pops one utterance, POSTs raw PCM to
//                       <backend>/transcribe, parses out the transcript and
//                       pushes it onto txt_queue.
//     merge_task      - Accumulates transcripts that arrive within 1500 ms of
//                       each other (or up to MAX_FRAGMENTS), then POSTs the
//                       merged text to <backend>/chat. The HTTPS body is a
//                       WAV (24 kHz mono Linear16). We strip the 44-byte
//                       header, upsample 24k -> 44.1k and duplicate to stereo
//                       on the fly, and stream samples into audio_ring with
//                       back-pressure so the BT source paces the download.
//
// One physical ESP32 dev board: INMP441 I2S mic, Wi-Fi to the Cloudflare
// Worker, and Bluetooth A2DP source to a normal BT speaker.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BluetoothA2DPSource.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Pins / Wi-Fi / server / Bluetooth
// ---------------------------------------------------------------------------

#define I2S_WS  25
#define I2S_SD  33
#define I2S_SCK 26

const char* WIFI_SSID = "GALGOTIAS-ARUBA";
const char* WIFI_PASS = "1234567@";

// Backend host (no path). All endpoints share this host.
static const char* BACKEND_HOST = "https://backend.dev1974sai.workers.dev";

// /transcribe describes the raw PCM body via query string. The Worker wraps
// the bytes in a WAV header before forwarding to Sarvam.
static const char* STT_PATH = "/transcribe?sample_rate=16000&channels=1"
                              "&bits_per_sample=16&language_code=en-IN&mode=transcribe";

// /chat takes JSON {deviceId, text} and returns audio/wav. We construct the
// deviceId from the MAC address at boot.
static const char* CHAT_PATH = "/chat";

// Exact name advertised by your Bluetooth speaker (case sensitive). The
// ESP32-A2DP library will scan for it and auto-connect. Change to match.
static const char* BT_SPEAKER_NAME = "JBL Flip 5";

// ---------------------------------------------------------------------------
// Audio + VAD configuration  (TUNE THESE)
// ---------------------------------------------------------------------------

static const int    SAMPLE_RATE   = 16000;
static const int    FRAME_MS      = 20;
static const size_t FRAME_SAMPLES = (SAMPLE_RATE * FRAME_MS) / 1000; // 320

// Cap on each captured utterance. Two ping-pong buffers of this size are
// allocated, so total RAM cost is 2 * MAX_UTTERANCE_SECONDS * 32 KB.
//   2 s = 128 KB total  (default, fits stock ESP32-WROOM after Bluedroid)
//   3 s = 192 KB total  (PSRAM strongly recommended)
//   8 s = 512 KB total  (PSRAM only)
static const int    MAX_UTTERANCE_SECONDS = 2;

static const int    PREROLL_FRAMES    = 15;     // 300 ms
static const int    HANGOVER_FRAMES   = 35;     // 700 ms
static const int    MIN_VOICED_FRAMES = 18;     // 360 ms
static const int    MIC_SHIFT         = 11;     // ~32x gain for INMP441

// ----- VAD thresholds ------------------------------------------------------

static const float    TRIGGER_SNR        = 3.0f;
static const float    RELEASE_SNR        = 1.8f;
static const int16_t  ABSOLUTE_MIN_PEAK  = 1200;
static const float    NOISE_EMA_ALPHA    = 0.05f;
static const float    NOISE_FLOOR_INIT   = 200.0f;
static const float    NOISE_FLOOR_MIN    = 80.0f;
static const float    NOISE_FLOOR_MAX    = 4000.0f;
static const int      NOISE_LOG_PERIOD   = 100;  // ~2 s

// ---------------------------------------------------------------------------
// Merge window (client side) - see plan section 6.3.
// Multiple short VAD utterances that arrive close together are concatenated
// at the transcript level and sent as one /chat turn.
// ---------------------------------------------------------------------------

static const uint32_t MERGE_WINDOW_MS = 1500; // inactivity flush
static const size_t   MAX_FRAGMENTS   = 6;    // hard cap per merged turn
static const size_t   MAX_MERGED_LEN  = 1024; // chars

// ---------------------------------------------------------------------------
// A2DP playback ring (FreeRTOS StreamBuffer of 44.1 kHz stereo int16 frames)
// 16 KB = ~93 ms of audio - small enough for low-latency reaction, large
// enough to absorb Wi-Fi/BT coexistence gaps.
// ---------------------------------------------------------------------------

static const size_t AUDIO_RING_BYTES   = 16 * 1024;
static const size_t AUDIO_RING_TRIGGER = 512; // bytes available before A2DP wakes consumer

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct FrameStats {
  uint16_t peak;
  float    rms;
};

enum VadState { VAD_IDLE, VAD_RECORDING };

// One captured utterance handed from vad_task to net_task. `slot` (0 or 1)
// tells net_task which ping-pong buffer to release when the upload is done.
struct UttJob {
  int16_t* pcm;
  size_t   samples;
  uint8_t  slot;
};

// One transcribed string handed from net_task to merge_task. Caller frees.
struct TranscriptJob {
  char* text;
};

// ---------------------------------------------------------------------------
// Buffers / state
// ---------------------------------------------------------------------------

// Two ping-pong utterance buffers so vad_task can record into one while
// net_task uploads from the other. `utt_slot_busy[i]` true while net_task
// holds slot i.
static int16_t* utt_buf[2]            = { nullptr, nullptr };
static volatile bool utt_slot_busy[2] = { false, false };
static size_t   utt_capacity_samples  = 0;
static size_t   utt_capacity_bytes    = 0;
static int      utt_capacity_seconds  = 0;

// Preroll for splicing audio just before the VAD trigger.
static int16_t* preroll_buf = nullptr;
static size_t   preroll_pos = 0;
static const size_t PREROLL_TOTAL_SAMPLES = (size_t)PREROLL_FRAMES * FRAME_SAMPLES;

// Queues + stream buffer.
static QueueHandle_t       utt_queue   = nullptr;
static QueueHandle_t       txt_queue   = nullptr;
static StreamBufferHandle_t audio_ring = nullptr;

// Bluetooth source.
static BluetoothA2DPSource a2dp_source;

// Stable per-device id (MAC-derived).
static char device_id[24] = {0};

// VAD state (only touched from vad_task).
static VadState vad_state           = VAD_IDLE;
static size_t   utt_samples         = 0;
static int      utt_voiced_frames   = 0;
static int      utt_hangover_frames = 0;
static float    noise_floor_rms     = NOISE_FLOOR_INIT;
static int      idle_log_counter    = 0;
static uint8_t  utt_active_slot     = 0;

// ---------------------------------------------------------------------------
// Allocator: PSRAM first, internal DRAM fallback. MALLOC_CAP_8BIT is required
// or the buffer might land in IRAM-tagged memory that only supports 32-bit
// aligned access (would crash with LoadStoreError on int16_t writes).
// ---------------------------------------------------------------------------
static void* tryAllocBytes(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ---------------------------------------------------------------------------
// Wi-Fi / I2S setup
// ---------------------------------------------------------------------------
static void connectWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // critical for low-latency HTTPS while BT is active
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  const unsigned long timeout_ms = 30000;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeout_ms) {
      Serial.printf("\nWiFi connect timed out (status=%d). Restarting...\n",
                    (int)WiFi.status());
      delay(500);
      ESP.restart();
    }
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

static void setupI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("Mic ready");
}

static void allocateBuffers() {
  Serial.printf("Heap at boot: free=%u, largest=%u, psram_free=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)ESP.getFreePsram());

  // Preroll first so the larger utterance pool gets the biggest contiguous
  // block left over.
  size_t preroll_bytes = PREROLL_TOTAL_SAMPLES * sizeof(int16_t);
  preroll_buf = (int16_t*)tryAllocBytes(preroll_bytes);
  if (!preroll_buf) {
    Serial.println("FATAL: preroll alloc failed");
    while (true) delay(1000);
  }
  memset(preroll_buf, 0, preroll_bytes);

  // Two ping-pong utterance buffers; walk down 1 second at a time until both
  // fit. We require BOTH to succeed at the same size or fall back together.
  for (int s = MAX_UTTERANCE_SECONDS; s >= 1; s--) {
    size_t samples = (size_t)SAMPLE_RATE * (size_t)s;
    size_t bytes   = samples * sizeof(int16_t);
    void* a = tryAllocBytes(bytes);
    void* b = a ? tryAllocBytes(bytes) : nullptr;
    if (a && b) {
      utt_buf[0] = (int16_t*)a;
      utt_buf[1] = (int16_t*)b;
      utt_capacity_samples = samples;
      utt_capacity_bytes   = bytes;
      utt_capacity_seconds = s;
      Serial.printf("utterance pool: 2 x %u bytes (%d s each). free=%u\n",
                    (unsigned)bytes, s, (unsigned)ESP.getFreeHeap());
      if (s != MAX_UTTERANCE_SECONDS) {
        Serial.printf("NOTE: requested %d s but only %d s fit per slot.\n",
                      MAX_UTTERANCE_SECONDS, s);
      }
      return;
    }
    if (a) heap_caps_free(a);
    if (b) heap_caps_free(b);
    Serial.printf("  %d s pair (%u bytes each) didn't fit, trying smaller...\n",
                  s, (unsigned)bytes);
  }

  Serial.println("FATAL: could not allocate even 1 s utterance pool. Use ESP32-WROVER (PSRAM).");
  while (true) delay(1000);
}

// ---------------------------------------------------------------------------
// Frame capture (vad_task helper)
// ---------------------------------------------------------------------------
static bool readFrame(int16_t* out, FrameStats* stats) {
  int32_t raw[FRAME_SAMPLES];
  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes_read, portMAX_DELAY);
  if (err != ESP_OK) {
    Serial.printf("i2s_read error: %d\n", err);
    return false;
  }
  size_t got = bytes_read / sizeof(int32_t);
  if (got == 0) return false;

  uint32_t peak = 0;
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < got; i++) {
    int32_t s = raw[i] >> MIC_SHIFT;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    int16_t s16 = (int16_t)s;
    out[i] = s16;
    uint32_t mag = (s16 < 0) ? (uint32_t)(-s16) : (uint32_t)s16;
    if (mag > peak) peak = mag;
    sum_sq += (uint64_t)mag * mag;
  }
  for (size_t i = got; i < FRAME_SAMPLES; i++) out[i] = 0;

  stats->peak = (uint16_t)((peak > 32767) ? 32767 : peak);
  stats->rms  = sqrtf((float)sum_sq / (float)got);
  return true;
}

static void pushPreroll(const int16_t* frame) {
  size_t pos = preroll_pos;
  for (size_t i = 0; i < FRAME_SAMPLES; i++) {
    preroll_buf[pos++] = frame[i];
    if (pos >= PREROLL_TOTAL_SAMPLES) pos = 0;
  }
  preroll_pos = pos;
}

static size_t flushPrerollIntoSlot(int16_t* dst) {
  size_t out = 0;
  size_t src = preroll_pos; // oldest sample is at the write index
  for (size_t i = 0; i < PREROLL_TOTAL_SAMPLES; i++) {
    dst[out++] = preroll_buf[src++];
    if (src >= PREROLL_TOTAL_SAMPLES) src = 0;
  }
  return out;
}

static int8_t pickFreeSlot() {
  for (int i = 0; i < 2; i++) if (!utt_slot_busy[i]) return (int8_t)i;
  return -1;
}

// ---------------------------------------------------------------------------
// vad_task: I2S read + VAD. Triggers an utterance into a free ping-pong slot
// and ships it to net_task on end-of-speech.
// ---------------------------------------------------------------------------
static void finalizeUtterance(const char* reason) {
  float voiced_ms = utt_voiced_frames * (float)FRAME_MS;
  float total_ms  = (utt_samples * 1000.0f) / (float)SAMPLE_RATE;

  if (utt_voiced_frames < MIN_VOICED_FRAMES) {
    Serial.printf("[utt] %s. voiced=%.0fms total=%.0fms -> DISCARD (<%dms)\n",
                  reason, voiced_ms, total_ms, MIN_VOICED_FRAMES * FRAME_MS);
    utt_slot_busy[utt_active_slot] = false; // release the slot we reserved
  } else {
    UttJob job = { utt_buf[utt_active_slot], utt_samples, utt_active_slot };
    Serial.printf("[utt] %s. voiced=%.0fms total=%.0fms -> queue slot %u\n",
                  reason, voiced_ms, total_ms, (unsigned)utt_active_slot);
    if (xQueueSend(utt_queue, &job, 0) != pdTRUE) {
      Serial.println("[utt] utt_queue full, dropping utterance");
      utt_slot_busy[utt_active_slot] = false;
    }
  }

  vad_state = VAD_IDLE;
  utt_samples = 0;
  utt_voiced_frames = 0;
  utt_hangover_frames = 0;
  idle_log_counter = 0;
}

static void vad_task(void*) {
  int16_t frame[FRAME_SAMPLES];
  FrameStats st;

  for (;;) {
    if (!readFrame(frame, &st)) continue;
    pushPreroll(frame);

    const float trigger_rms = noise_floor_rms * TRIGGER_SNR;
    const float release_rms = noise_floor_rms * RELEASE_SNR;

    if (vad_state == VAD_IDLE) {
      bool loud_enough = (st.peak >= ABSOLUTE_MIN_PEAK);
      bool above_noise = (st.rms  >= trigger_rms);

      if (loud_enough && above_noise) {
        int8_t slot = pickFreeSlot();
        if (slot < 0) {
          Serial.println("[vad] both utterance slots busy, dropping trigger");
          // adapt noise floor instead so we don't spin
          noise_floor_rms = (1.0f - NOISE_EMA_ALPHA) * noise_floor_rms +
                            NOISE_EMA_ALPHA * st.rms;
          continue;
        }
        utt_active_slot = (uint8_t)slot;
        utt_slot_busy[slot] = true;
        utt_samples = flushPrerollIntoSlot(utt_buf[slot]);
        utt_voiced_frames = 1;
        utt_hangover_frames = 0;
        vad_state = VAD_RECORDING;
        Serial.printf("[trigger] rms=%.0f peak=%u noise=%.0f snr=%.1fx slot=%d\n",
                      st.rms, st.peak, noise_floor_rms,
                      st.rms / (noise_floor_rms + 1e-3f), slot);
      } else {
        noise_floor_rms = (1.0f - NOISE_EMA_ALPHA) * noise_floor_rms +
                          NOISE_EMA_ALPHA * st.rms;
        if (noise_floor_rms < NOISE_FLOOR_MIN) noise_floor_rms = NOISE_FLOOR_MIN;
        if (noise_floor_rms > NOISE_FLOOR_MAX) noise_floor_rms = NOISE_FLOOR_MAX;
        if (++idle_log_counter >= NOISE_LOG_PERIOD) {
          idle_log_counter = 0;
          Serial.printf("[idle] noise=%.0f rms=%.0f peak=%u heap=%u\n",
                        noise_floor_rms, st.rms, st.peak,
                        (unsigned)ESP.getFreeHeap());
        }
      }
      continue;
    }

    // VAD_RECORDING
    if (utt_samples + FRAME_SAMPLES > utt_capacity_samples) {
      finalizeUtterance("buffer full");
      continue;
    }
    memcpy(&utt_buf[utt_active_slot][utt_samples], frame,
           FRAME_SAMPLES * sizeof(int16_t));
    utt_samples += FRAME_SAMPLES;

    if (st.rms >= release_rms || st.peak >= ABSOLUTE_MIN_PEAK) {
      utt_voiced_frames++;
      utt_hangover_frames = 0;
    } else {
      utt_hangover_frames++;
      if (utt_hangover_frames >= HANGOVER_FRAMES) {
        finalizeUtterance("end of speech");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Tiny JSON-string scraper (avoids ArduinoJson dependency for two fields)
// ---------------------------------------------------------------------------
static String extractJsonString(const String& body, const char* key) {
  String needle = String('"') + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return String();
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return String();
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return String();
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 <= q1) return String();
  return body.substring(q1 + 1, q2);
}

// ---------------------------------------------------------------------------
// net_task: pop utterance, POST raw PCM to /transcribe, push transcript
// ---------------------------------------------------------------------------
static void net_task(void*) {
  WiFiClientSecure tls;
  tls.setInsecure(); // prototype: skip cert validation; ship a real CA bundle for prod
  String url = String(BACKEND_HOST) + STT_PATH;

  for (;;) {
    UttJob job;
    if (xQueueReceive(utt_queue, &job, portMAX_DELAY) != pdTRUE) continue;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[net] WiFi dropped, reconnecting...");
      connectWifi();
    }

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    if (!http.begin(tls, url)) {
      Serial.println("[net] http.begin failed");
      utt_slot_busy[job.slot] = false;
      continue;
    }
    http.addHeader("Content-Type", "audio/pcm");
    http.addHeader("Accept", "application/json");

    size_t bytes_to_send = job.samples * sizeof(int16_t);
    Serial.printf("[net] POST /transcribe %u bytes (%.2fs)\n",
                  (unsigned)bytes_to_send,
                  (float)job.samples / (float)SAMPLE_RATE);

    unsigned long t0 = millis();
    int code = http.POST((uint8_t*)job.pcm, bytes_to_send);
    unsigned long elapsed = millis() - t0;
    String body = http.getString();
    http.end();

    // Slot can be reused by vad_task immediately - we already have body.
    utt_slot_busy[job.slot] = false;

    if (code <= 0) {
      Serial.printf("[net] HTTP transport error %d (%s) in %lu ms\n",
                    code, HTTPClient::errorToString(code).c_str(), elapsed);
      continue;
    }
    if (code != 200) {
      Serial.printf("[net] HTTP %d in %lu ms: %s\n", code, elapsed, body.c_str());
      continue;
    }

    String transcript = extractJsonString(body, "transcript");
    transcript.trim();
    if (transcript.length() == 0) {
      Serial.printf("[net] empty transcript in %lu ms (body=%s)\n",
                    elapsed, body.c_str());
      continue;
    }
    Serial.printf("[net] transcript (%lu ms): %s\n", elapsed, transcript.c_str());

    // Hand off to merge_task. Allocate a copy on the heap so the queue can
    // own the lifetime; merge_task frees after consuming.
    TranscriptJob tj;
    tj.text = (char*)malloc(transcript.length() + 1);
    if (!tj.text) {
      Serial.println("[net] OOM allocating transcript copy");
      continue;
    }
    memcpy(tj.text, transcript.c_str(), transcript.length() + 1);
    if (xQueueSend(txt_queue, &tj, pdMS_TO_TICKS(50)) != pdTRUE) {
      Serial.println("[net] txt_queue full, dropping transcript");
      free(tj.text);
    }
  }
}

// ---------------------------------------------------------------------------
// Audio playback pipeline (called from merge_task while reading /chat body)
// ---------------------------------------------------------------------------

// Resampler state - reset at the start of every reply.
static float    rs_phase = 0.0f;
static int16_t  rs_prev  = 0;

static void resamplerReset() {
  rs_phase = 0.0f;
  rs_prev = 0;
}

// Push `n` mono int16 samples @ 24 kHz into the audio_ring as 44.1 kHz stereo.
// Blocks if the ring is full so the HTTP read paces with playback.
static void pushUpsampled(const int16_t* mono24k, size_t n) {
  // Output ratio: we step `phase` by `step` per output sample. When phase
  // crosses 1.0 we advance to the next input sample.
  const float step = 24000.0f / 44100.0f; // ~0.5442
  Frame out[64];                          // 64 stereo frames buffer
  size_t out_idx = 0;

  for (size_t i = 0; i < n; i++) {
    int16_t cur = mono24k[i];
    while (rs_phase < 1.0f) {
      int16_t s = (int16_t)((float)rs_prev + ((float)cur - (float)rs_prev) * rs_phase);
      out[out_idx].channel1 = s;
      out[out_idx].channel2 = s;
      out_idx++;
      if (out_idx == 64) {
        xStreamBufferSend(audio_ring, out, sizeof(out), portMAX_DELAY);
        out_idx = 0;
      }
      rs_phase += step;
    }
    rs_phase -= 1.0f;
    rs_prev = cur;
  }
  if (out_idx) {
    xStreamBufferSend(audio_ring, out, out_idx * sizeof(Frame), portMAX_DELAY);
  }
}

// Called from the BT source task. Must be fast and non-blocking; fill silence
// for any bytes the ring doesn't have right now.
static int32_t a2dp_data_cb(Frame* frame, int32_t frame_count) {
  size_t needed = (size_t)frame_count * sizeof(Frame);
  size_t got = xStreamBufferReceive(audio_ring, frame, needed, 0);
  if (got < needed) {
    memset((uint8_t*)frame + got, 0, needed - got);
  }
  return frame_count;
}

// ---------------------------------------------------------------------------
// /chat HTTP: POST merged text, stream the WAV body straight into audio_ring.
// ---------------------------------------------------------------------------

// JSON-escape a string into `out`. Returns true on success. Worst-case 6x.
static bool jsonEscapeInto(String& out, const char* s) {
  out = "";
  if (!out.reserve(strlen(s) + 16)) return false;
  for (const char* p = s; *p; p++) {
    char c = *p;
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return true;
}

static void postChatAndStream(const String& merged) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[chat] WiFi dropped, reconnecting...");
    connectWifi();
  }

  WiFiClientSecure tls;
  tls.setInsecure();

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(30000);
  String url = String(BACKEND_HOST) + CHAT_PATH;
  if (!http.begin(tls, url)) {
    Serial.println("[chat] http.begin failed");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "audio/wav");

  // We want to read X-Reply-Text from the response.
  static const char* kResponseHeaders[] = { "X-Reply-Text" };
  http.collectHeaders(kResponseHeaders, 1);

  // Build request JSON: {"deviceId":"<mac>", "text":"<merged>"}
  String escaped;
  jsonEscapeInto(escaped, merged.c_str());
  String body;
  body.reserve(escaped.length() + 64);
  body  = "{\"deviceId\":\"";
  body += device_id;
  body += "\",\"text\":\"";
  body += escaped;
  body += "\"}";

  Serial.printf("[chat] POST /chat (%u text bytes)\n", (unsigned)body.length());
  unsigned long t0 = millis();
  int code = http.POST(body);
  unsigned long t_resp = millis() - t0;

  if (code <= 0) {
    Serial.printf("[chat] transport error %d (%s) in %lu ms\n",
                  code, HTTPClient::errorToString(code).c_str(), t_resp);
    http.end();
    return;
  }
  if (code != 200) {
    String err = http.getString();
    Serial.printf("[chat] HTTP %d in %lu ms: %s\n", code, t_resp, err.c_str());
    http.end();
    return;
  }

  // Log the X-Reply-Text header (URL-encoded preview) for debug.
  String replyHdr = http.header("X-Reply-Text");
  if (replyHdr.length()) {
    Serial.printf("[chat] reply preview (urlenc): %s\n", replyHdr.c_str());
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("[chat] no body stream");
    http.end();
    return;
  }

  // ---- Parse 44-byte WAV header ----
  uint8_t hdr[44];
  size_t hdr_read = 0;
  unsigned long hdr_deadline = millis() + 8000;
  while (hdr_read < sizeof(hdr) && millis() < hdr_deadline) {
    if (stream->available()) {
      int n = stream->read(hdr + hdr_read, sizeof(hdr) - hdr_read);
      if (n > 0) hdr_read += n;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  if (hdr_read != sizeof(hdr) ||
      memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    Serial.println("[chat] bad WAV header");
    http.end();
    return;
  }
  uint16_t fmt_channels = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
  uint32_t fmt_rate     = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                          ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
  uint16_t fmt_bits     = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);

  if (fmt_channels != 1 || fmt_rate != 24000 || fmt_bits != 16) {
    Serial.printf("[chat] unexpected WAV format: ch=%u rate=%u bits=%u\n",
                  fmt_channels, fmt_rate, fmt_bits);
    http.end();
    return;
  }

  // ---- Stream samples through the resampler into audio_ring ----
  resamplerReset();
  static const size_t CHUNK = 1024; // 512 mono samples = 21.3 ms @ 24k
  uint8_t buf[CHUNK];
  int16_t mono[CHUNK / 2];
  size_t total_pcm_bytes = 0;
  unsigned long stream_deadline = millis() + 30000;
  unsigned long t_first_audio = 0;

  // Use Content-Length to know exactly how many PCM bytes follow the header,
  // so we don't truncate the tail when the remote closes the socket but
  // bytes still sit in the local TCP buffer.
  int content_length = http.getSize(); // -1 if chunked / unknown
  size_t pcm_remaining = (content_length > (int)sizeof(hdr))
                           ? (size_t)(content_length - (int)sizeof(hdr))
                           : 0;

  while (millis() < stream_deadline) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected()) break; // remote closed and local buffer drained
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t want = (size_t)avail;
    if (want > CHUNK) want = CHUNK;
    if (pcm_remaining > 0 && want > pcm_remaining) want = pcm_remaining;
    if (want & 1) want--;
    if (want == 0) continue;

    int n = stream->read(buf, want);
    if (n <= 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (n & 1) n--;
    if (n <= 0) continue;
    if (!t_first_audio) t_first_audio = millis();

    size_t mono_count = (size_t)n / 2;
    for (size_t i = 0; i < mono_count; i++) {
      mono[i] = (int16_t)((uint16_t)buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8));
    }
    pushUpsampled(mono, mono_count);
    total_pcm_bytes += (size_t)n;
    if (pcm_remaining > 0) {
      pcm_remaining -= (size_t)n;
      if (pcm_remaining == 0) break;
    }
  }

  http.end();
  Serial.printf("[chat] done: %u PCM bytes (cl=%d, remain=%u), total %lu ms, first audio +%lu ms\n",
                (unsigned)total_pcm_bytes, content_length, (unsigned)pcm_remaining,
                millis() - t0, t_first_audio ? (t_first_audio - t0) : 0);
}

// ---------------------------------------------------------------------------
// merge_task: collect transcripts within MERGE_WINDOW_MS, fire to /chat
// ---------------------------------------------------------------------------
static void merge_task(void*) {
  String pending;
  pending.reserve(MAX_MERGED_LEN + 16);
  uint32_t last_at = 0;
  size_t fragments = 0;

  for (;;) {
    TranscriptJob tj;
    BaseType_t got = xQueueReceive(txt_queue, &tj, pdMS_TO_TICKS(150));
    uint32_t now = millis();

    if (got == pdTRUE) {
      if (pending.length() && pending.length() + 1 + strlen(tj.text) < MAX_MERGED_LEN) {
        pending += ' ';
      }
      if (pending.length() + strlen(tj.text) < MAX_MERGED_LEN) {
        pending += tj.text;
      }
      free(tj.text);
      last_at = now;
      fragments++;
      Serial.printf("[merge] +fragment (%u total): %s\n",
                    (unsigned)fragments, pending.c_str());

      if (fragments >= MAX_FRAGMENTS) {
        Serial.println("[merge] MAX_FRAGMENTS reached -> flush");
        postChatAndStream(pending);
        pending = "";
        fragments = 0;
        last_at = 0;
      }
      continue;
    }

    if (pending.length() && (now - last_at) >= MERGE_WINDOW_MS) {
      Serial.printf("[merge] %lu ms idle -> flush: %s\n",
                    (unsigned long)(now - last_at), pending.c_str());
      postChatAndStream(pending);
      pending = "";
      fragments = 0;
      last_at = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// setup / startup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  allocateBuffers();

  audio_ring = xStreamBufferCreate(AUDIO_RING_BYTES, AUDIO_RING_TRIGGER);
  utt_queue  = xQueueCreate(2, sizeof(UttJob));        // depth 2 = both slots in flight
  txt_queue  = xQueueCreate(MAX_FRAGMENTS + 2, sizeof(TranscriptJob));

  if (!audio_ring || !utt_queue || !txt_queue) {
    Serial.println("FATAL: queue/streambuffer alloc failed");
    while (true) delay(1000);
  }

  // device_id from MAC, e.g. "esp32-A8032AB3CC10"
  uint64_t mac = ESP.getEfuseMac();
  snprintf(device_id, sizeof(device_id),
           "esp32-%02X%02X%02X%02X%02X%02X",
           (uint8_t)(mac >> 0),  (uint8_t)(mac >> 8),
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 24),
           (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  Serial.printf("device_id = %s\n", device_id);

  connectWifi();
  setupI2S();

  // Bring up A2DP source. The data callback fills 44.1 kHz stereo frames
  // from audio_ring (silence when empty). The library spawns its own task
  // pinned to core 0 at high priority.
  a2dp_source.set_auto_reconnect(true);
  a2dp_source.set_data_callback_in_frames(a2dp_data_cb);
  a2dp_source.start(BT_SPEAKER_NAME);
  Serial.printf("A2DP source started, scanning for '%s'...\n", BT_SPEAKER_NAME);

  // Tasks
  xTaskCreatePinnedToCore(vad_task,   "vad",   8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(net_task,   "net",   8192, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(merge_task, "merge", 8192, nullptr, 3, nullptr, 1);

  Serial.printf(
    "Voice agent ready. frame=%dms preroll=%dms hangover=%dms min_voice=%dms "
    "trigger_snr=%.1fx min_peak=%d max_utt=%ds merge_window=%lums max_fragments=%u\n",
    FRAME_MS, PREROLL_FRAMES * FRAME_MS, HANGOVER_FRAMES * FRAME_MS,
    MIN_VOICED_FRAMES * FRAME_MS, (double)TRIGGER_SNR, (int)ABSOLUTE_MIN_PEAK,
    utt_capacity_seconds, (unsigned long)MERGE_WINDOW_MS, (unsigned)MAX_FRAGMENTS);
}

// All work happens in the FreeRTOS tasks; the Arduino main loop just yields.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
