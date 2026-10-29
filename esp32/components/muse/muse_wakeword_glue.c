/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_wakeword_glue.h"

#include "sdkconfig.h"

#if CONFIG_MUSE_WAKEWORD_ENABLE

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wakeword.h"
#include "ww_runner.h"
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
#include "ww_runner_bench.h"
#endif

static const char *TAG = "muse_wake";

#define TAP_CHUNKS 25                                  /* §5.2: 500 ms of scheduling slack */
#define PRE_FRAMES (MUSE_AUDIO_RATE)                   /* §5.3: 1 s of upload pre-roll */
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
#define WAKE_TASK_STACK 4096
#else
#define WAKE_TASK_STACK 8192   /* ET feed+execute peak measured 2.8 KiB of 16 KiB
                                * on BOX-3 (ww_et timing log, stack free 13580 B) */
#endif
#define WAKE_TASK_PRIO 5                               /* below muse_voice (6), §10.2 */

static QueueHandle_t s_queue;
static ww_runner_t *s_runner;
static muse_wakeword_core_t s_core;
static muse_ww_ring_t s_tap;    /* voice_task -> wake task (§5.2) */
static muse_ww_ring_t s_pre;    /* wake task only (§5.3) */
static int16_t *s_snap;         /* PRE_FRAMES, for muse_voice_seed_preroll */
static bool s_started;
static volatile bool s_enabled;
static volatile bool s_playback;
static volatile bool s_ptt_turn;
static volatile bool s_pending_down, s_pending_up;   /* physical, from muse_voice */
static uint32_t s_capture_chunks;                    /* wake task only, for the log */
static size_t s_seeded_frames;

const char *muse_wakeword_runner_kind(void)
{
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
    return "bench";
#else
    return "executorch";
#endif
}

bool muse_wakeword_enabled(void)
{
    return s_started && s_enabled;
}

bool muse_wakeword_listening(void)
{
    return s_started && s_enabled && !s_playback && !s_ptt_turn && !muse_voice_resting();
}

void muse_wakeword_set_ptt_turn(bool active)
{
    if (!s_started || active == s_ptt_turn) {
        return;
    }
    s_ptt_turn = active;
    if (!active) {
        muse_ww_ring_clear(&s_pre);
        ww_runner_reset(s_runner);
    }
}

void muse_wakeword_on_audio(const int16_t *pcm, size_t frames)
{
    if (!s_started || !s_enabled) {
        return;   /* one branch while off: the §5.1 cost contract */
    }
    muse_ww_ring_push(&s_tap, pcm, frames);
}

void muse_wakeword_note_ptt(bool down)
{
    if (down) {
        s_pending_down = true;
    } else {
        s_pending_up = true;
    }
}

void muse_wakeword_set_playback(bool on)
{
    if (!s_started || on == s_playback) {
        return;
    }
    s_playback = on;
    ww_runner_set_suppressed(s_runner, on);
    muse_ww_ring_clear(&s_pre);   /* Muse's own output is never pre-roll */
    if (!on) {
        ww_runner_reset(s_runner);   /* no stale model state across turns */
    }
}

void muse_wakeword_set_enabled(bool on)
{
    muse_settings_set_wakeword_on(on);
    if (!s_started) {
        return;
    }
    if (on) {
        muse_ww_ring_clear(&s_tap);
        muse_ww_ring_clear(&s_pre);
        muse_wakeword_core_reset(&s_core);
        s_pending_down = s_pending_up = false;
        ww_runner_reset(s_runner);
    } else {
        muse_ww_ring_clear(&s_pre);
        /* The core closes an open capture on its next ticks with
         * enabled=false; the task keeps ticking until it is idle. */
    }
    s_enabled = on;
    ww_runner_set_listening(s_runner, on);
    ESP_LOGI(TAG, "listening %s (runner=%s)", on ? "on" : "off", muse_wakeword_runner_kind());
}

void muse_wakeword_bench_trigger(bool with_speech)
{
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
    if (!s_started) {
        return;
    }
    if (with_speech) {
        ww_runner_bench_trigger_test(s_runner);
    } else {
        ww_runner_bench_trigger(s_runner);
    }
    ESP_LOGI(TAG, "bench trigger%s queued", with_speech ? " (with speech)" : "");
#else
    (void)with_speech;
    ESP_LOGW(TAG, "bench trigger ignored: runner=%s has no triggers", muse_wakeword_runner_kind());
#endif
}

static void post_ptt(muse_ptt_t type)
{
    muse_input_event_t ev = { .type = type, .synthetic = true };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "input queue full, synthetic %s dropped",
                 type == MUSE_PTT_DOWN ? "down" : "up");
    }
}

/* One core tick and everything its actions mean for the device. */
static void tick(const muse_wakeword_input_t *in)
{
    muse_ww_state_t before = muse_wakeword_core_state(&s_core);
    uint32_t act = muse_wakeword_core_tick(&s_core, in);
    muse_ww_state_t now = muse_wakeword_core_state(&s_core);
    if (now == MUSE_WW_CAPTURING || now == MUSE_WW_CAPTURING_PTT) {
        s_capture_chunks++;
    }
    if (before == MUSE_WW_IDLE && now == MUSE_WW_ARMED) {
        ESP_LOGI(TAG, "wake heard (runner=%s), listening for speech", muse_wakeword_runner_kind());
        muse_state_poke();
        muse_state_set_caption("HEY MUSE HEARD - SPEAK NOW");
    }
    if (act & MUSE_WW_ACTION_POST_PTT_DOWN) {
        s_seeded_frames = muse_ww_ring_snapshot(&s_pre, s_snap, PRE_FRAMES);
        muse_voice_seed_preroll(s_snap, s_seeded_frames);
        s_capture_chunks = 0;
        /* Show the capture state here, before posting: record() sets the
         * same state when it accepts the turn, and replaces it with the
         * reason when it cannot (for example, Muse is not set up yet). */
        muse_state_set_mode(MUSE_MODE_LISTENING);
        muse_state_set_caption("LISTENING...");
        post_ptt(MUSE_PTT_DOWN);
        ESP_LOGI(TAG, "speech started, pre-roll %.2fs", (double)s_seeded_frames / MUSE_AUDIO_RATE);
    }
    if (act & MUSE_WW_ACTION_POST_PTT_UP) {
        post_ptt(MUSE_PTT_UP);
    }
    if (act & MUSE_WW_ACTION_CONVERTED_TO_PTT) {
        ESP_LOGI(TAG, "the talk button took over");
    }
    if (act & MUSE_WW_ACTION_SUBMITTED) {
        ESP_LOGI(TAG, "wake: captured %.1fs, preroll %.1fs",
                 s_capture_chunks * (double)MUSE_AUDIO_CHUNK / MUSE_AUDIO_RATE,
                 (double)s_seeded_frames / MUSE_AUDIO_RATE);
    }
    if (act & MUSE_WW_ACTION_CANCELLED) {
        if (before == MUSE_WW_ARMED) {
            muse_state_set_caption("%s", "");   /* ours to clear; no churn beyond that */
        }
        ESP_LOGI(TAG, "wake ignored (no speech followed)");
    }
}

static muse_wakeword_input_t next_input(const int16_t *pcm)
{
    /* One suppression gate for the core: own-voice playback and a
     * physical turn both mean "discard wakes" (§4.2). */
    bool suppressed = s_playback || s_ptt_turn;
    muse_wakeword_input_t in = {
        .voiced = muse_wakeword_energy_voiced(pcm, MUSE_AUDIO_CHUNK, MUSE_WAKEWORD_ENERGY_RMS_DEFAULT),
        .ptt_down = s_pending_down,
        .ptt_up = s_pending_up,
        .playback = suppressed,
        .enabled = s_enabled,
    };
    s_pending_down = s_pending_up = false;
    ww_runner_set_listening(s_runner, in.enabled);
    ww_runner_set_suppressed(s_runner, in.playback);
    in.event = ww_runner_feed(s_runner, pcm, MUSE_AUDIO_CHUNK);
    return in;
}

static void wake_task(void *arg)
{
    (void)arg;
    static int16_t chunk[MUSE_AUDIO_CHUNK];
    for (;;) {
        if (muse_ww_ring_fill(&s_tap) < MUSE_AUDIO_CHUNK) {
            /* Disabled mid-capture with the tap now dry: one silent
             * tick is what closes the synthetic hold. */
            if (!s_enabled && muse_wakeword_core_state(&s_core) != MUSE_WW_IDLE) {
                muse_wakeword_input_t in = { 0 };
                tick(&in);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (muse_ww_ring_pop(&s_tap, chunk, MUSE_AUDIO_CHUNK) != MUSE_AUDIO_CHUNK) {
            continue;
        }
        if (!s_playback && !s_ptt_turn) {
            muse_ww_ring_push(&s_pre, chunk, MUSE_AUDIO_CHUNK);
        }
        muse_wakeword_input_t in = next_input(chunk);
        tick(&in);
    }
}

esp_err_t muse_wakeword_init(QueueHandle_t queue)
{
    if (s_started) {
        return ESP_OK;
    }
    if (MUSE_AUDIO_RATE != 16000) {   /* §3.3 front-end identity */
        ESP_LOGE(TAG, "audio rate %d is not the runner's 16000", MUSE_AUDIO_RATE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_queue = queue;
    int16_t *tap_storage = heap_caps_malloc(TAP_CHUNKS * MUSE_AUDIO_CHUNK * sizeof(int16_t), MUSE_BIG_CAPS);
    int16_t *pre_storage = heap_caps_malloc(PRE_FRAMES * sizeof(int16_t), MUSE_BIG_CAPS);
    s_snap = heap_caps_malloc(PRE_FRAMES * sizeof(int16_t), MUSE_BIG_CAPS);
    ww_config_t cfg = {
        .sample_rate_hz = MUSE_AUDIO_RATE,
#if CONFIG_MUSE_WAKEWORD_SENSITIVITY_LOW
        .sensitivity = WW_SENSITIVITY_LOW,
#elif CONFIG_MUSE_WAKEWORD_SENSITIVITY_HIGH
        .sensitivity = WW_SENSITIVITY_HIGH,
#else
        .sensitivity = WW_SENSITIVITY_DEFAULT,
#endif
        .vad = WW_VAD_ENERGY,
    };
    s_runner = (tap_storage && pre_storage && s_snap) ? ww_runner_create(&cfg) : NULL;
    if (!s_runner) {
        free(tap_storage);
        free(pre_storage);
        free(s_snap);
        s_snap = NULL;
        ESP_LOGE(TAG, "no memory for wake word");
        return ESP_ERR_NO_MEM;
    }
    muse_ww_ring_init(&s_tap, tap_storage, TAP_CHUNKS * MUSE_AUDIO_CHUNK);
    muse_ww_ring_init(&s_pre, pre_storage, PRE_FRAMES);
    muse_wakeword_config_t core_cfg = {
        .start_timeout_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_START_TIMEOUT_MS),
        .end_silence_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_END_SILENCE_MS),
        .max_capture_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_MAX_CAPTURE_MS),
        .energy_endpointing = true,
    };
    muse_wakeword_core_init(&s_core, &core_cfg);
    s_enabled = muse_settings_wakeword_on();
    ww_runner_set_listening(s_runner, s_enabled);
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", WAKE_TASK_STACK, NULL,
                                        WAKE_TASK_PRIO, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the wake task");
        return ESP_ERR_NO_MEM;   /* s_started stays false: every hook no-ops */
    }
    s_started = true;
    ESP_LOGI(TAG, "wake word ready: runner=%s, listening %s", muse_wakeword_runner_kind(),
             s_enabled ? "on (saved opt-in)" : "off (enable_wakeword turns it on)");
    return ESP_OK;
}

void muse_wakeword_status(muse_wakeword_status_t *out)
{
    if (!out) {
        return;
    }
    muse_wakeword_stats_t st = muse_wakeword_core_stats(&s_core);
    *out = (muse_wakeword_status_t){
        .enabled = muse_wakeword_enabled(),
        .listening = muse_wakeword_listening(),
        .runner = muse_wakeword_runner_kind(),
        .state = (int)muse_wakeword_core_state(&s_core),
        .wakes = st.wakes,
        .cancels = st.cancels,
        .submits = st.submits,
        .conversions = st.conversions,
        .suppressed_wakes = st.suppressed_wakes,
        .dropped_frames = muse_ww_ring_drops(&s_tap),
    };
}

#else   /* !CONFIG_MUSE_WAKEWORD_ENABLE: stubs, see the header */

esp_err_t muse_wakeword_init(QueueHandle_t queue)
{
    (void)queue;
    return ESP_OK;
}
void muse_wakeword_on_audio(const int16_t *pcm, size_t frames)
{
    (void)pcm;
    (void)frames;
}
void muse_wakeword_note_ptt(bool down) { (void)down; }
void muse_wakeword_set_enabled(bool on) { (void)on; }
bool muse_wakeword_enabled(void) { return false; }
bool muse_wakeword_listening(void) { return false; }
void muse_wakeword_set_playback(bool on) { (void)on; }
void muse_wakeword_set_ptt_turn(bool active) { (void)active; }
const char *muse_wakeword_runner_kind(void) { return "off"; }
void muse_wakeword_bench_trigger(bool with_speech) { (void)with_speech; }
void muse_wakeword_status(muse_wakeword_status_t *out)
{
    if (out) {
        *out = (muse_wakeword_status_t){ .runner = "off" };
    }
}

#endif
