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

#include "ww_runner.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <et_model.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ww_detector.h"
#include "ww_frontend.h"

extern "C" {
extern const uint8_t _binary_hey_muse_pte_start[];
extern const uint8_t _binary_hey_muse_pte_end[];
}

namespace
{

const char *kTag = "ww_et";
/* ExecuTorch Program::load requires 8-byte-aligned program data. */
constexpr size_t kPteAlignment = 16;
constexpr size_t kMethodArenaBytes = 64 * 1024;
constexpr size_t kInputBytes = WW_FRONTEND_N_MELS * sizeof(float);
constexpr size_t kOutputBytes = 3 * sizeof(float);

} // namespace

struct ww_runner {
    ww_config_t cfg;
    ww_frontend_t *frontend = nullptr;
    ww_detector_t detector;
    et_model_t *model = nullptr;
    uint8_t *pte_copy = nullptr;   /* owned aligned .pte copy, if the embed was misaligned */
    std::mutex mutex;
    bool listening = true;
    bool suppressed = false;
    bool ready = false;
    bool fired = false;
    bool error_logged = false;
    /* Bring-up timing telemetry: per-window frontend + inference cost. */
    uint64_t push_us = 0;
    uint64_t infer_us = 0;
    uint32_t infer_us_max = 0;
    uint32_t infer_count = 0;
};

static void ww_runner_et_reset_locked(ww_runner_t *runner, bool reload_model)
{
    ww_frontend_reset(runner->frontend);
    ww_detector_reset(&runner->detector);
    runner->fired = false;
    if (reload_model && runner->model != nullptr) {
        runner->ready = et_model_load_method(runner->model, "forward");
        if (!runner->ready && !runner->error_logged) {
            ESP_LOGE(kTag, "model reload failed: %s", et_model_last_error(runner->model));
            runner->error_logged = true;
        }
    }
}

extern "C" void ww_runner_et_frame_cb(void *ctx, const float frame[WW_FRONTEND_N_MELS])
{
    auto *runner = static_cast<ww_runner_t *>(ctx);
    if (runner == nullptr || !runner->ready || runner->model == nullptr) {
        return;
    }

    const int64_t infer_t0 = esp_timer_get_time();
    if (!et_model_set_input(runner->model, 0, frame, kInputBytes) ||
        !et_model_execute(runner->model)) {
        runner->ready = false;
        if (!runner->error_logged) {
            ESP_LOGE(kTag, "inference failed: %s", et_model_last_error(runner->model));
            runner->error_logged = true;
        }
        return;
    }

    size_t out_bytes = 0;
    const void *out = et_model_output_data(runner->model, 0, &out_bytes);
    if (out == nullptr || out_bytes != kOutputBytes) {
        runner->ready = false;
        if (!runner->error_logged) {
            ESP_LOGE(kTag, "unexpected model output (%zu bytes)", out_bytes);
            runner->error_logged = true;
        }
        return;
    }

    const uint32_t infer_dt = (uint32_t)(esp_timer_get_time() - infer_t0);
    runner->infer_us += infer_dt;
    if (infer_dt > runner->infer_us_max) {
        runner->infer_us_max = infer_dt;
    }
    if (++runner->infer_count >= 250) {   /* 5 s of audio per window */
        ESP_LOGI(kTag,
                 "timing: %lu infer, avg %.2f ms, max %.2f ms; push (frontend+infer) "
                 "avg %.2f ms; stack free %u B",
                 (unsigned long)runner->infer_count,
                 (double)runner->infer_us / runner->infer_count / 1000.0,
                 (double)runner->infer_us_max / 1000.0,
                 (double)runner->push_us / runner->infer_count / 1000.0,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        runner->infer_us = 0;
        runner->infer_us_max = 0;
        runner->push_us = 0;
        runner->infer_count = 0;
    }

    float posterior = static_cast<const float *>(out)[0];
    if (!std::isfinite(posterior)) {
        posterior = 0.0f;
    }
    if (posterior < 0.0f) {
        posterior = 0.0f;
    } else if (posterior > 1.0f) {
        posterior = 1.0f;
    }

    if (ww_detector_push(&runner->detector, posterior)) {
        runner->fired = true;
        ESP_LOGI(kTag, "wake posterior %.3f", (double)posterior);
    }
}

extern "C" {

ww_runner_t *ww_runner_create(const ww_config_t *cfg)
{
    ww_config_t effective = {};
    if (cfg != nullptr) {
        effective = *cfg;
    } else {
        effective.sample_rate_hz = WW_FRONTEND_SAMPLE_RATE_HZ;
        effective.sensitivity = WW_SENSITIVITY_DEFAULT;
        effective.vad = WW_VAD_ENERGY;
    }

    if (effective.sample_rate_hz != WW_FRONTEND_SAMPLE_RATE_HZ) {
        ESP_LOGE(kTag, "unsupported sample rate %lu (need %d)",
                 (unsigned long)effective.sample_rate_hz, WW_FRONTEND_SAMPLE_RATE_HZ);
        return nullptr;
    }
    if (effective.vad != WW_VAD_ENERGY) {
        ESP_LOGE(kTag, "Silero VAD is not implemented in this runner");
        return nullptr;
    }

    const uint8_t *pte = effective.model;
    size_t pte_len = effective.model_len;
    if (pte == nullptr || pte_len == 0) {
        pte = _binary_hey_muse_pte_start;
        pte_len = (size_t)(_binary_hey_muse_pte_end - _binary_hey_muse_pte_start);
    }

    /* EMBED_FILES does not guarantee the alignment Program::load
     * requires, and rodata layout shifts between builds: copy a
     * misaligned .pte into owned aligned storage (et_model keeps
     * pointing into it, so the copy lives as long as the runner). */
    uint8_t *pte_copy = nullptr;
    if ((reinterpret_cast<uintptr_t>(pte) % kPteAlignment) != 0) {
        pte_copy = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(kPteAlignment, pte_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pte_copy == nullptr) {
            pte_copy = static_cast<uint8_t *>(
                heap_caps_aligned_alloc(kPteAlignment, pte_len, MALLOC_CAP_DEFAULT));
        }
        if (pte_copy == nullptr) {
            ESP_LOGE(kTag, "no memory for aligned pte copy (%zu B)", pte_len);
            return nullptr;
        }
        memcpy(pte_copy, pte, pte_len);
        ESP_LOGW(kTag, "pte at %p is not %zu-aligned; copied %zu B", (const void *)pte,
                 kPteAlignment, pte_len);
        pte = pte_copy;
    }

    auto *runner = new (std::nothrow) ww_runner_t();
    if (runner == nullptr) {
        free(pte_copy);
        return nullptr;
    }
    runner->cfg = effective;
    runner->pte_copy = pte_copy;
    runner->frontend = ww_frontend_create();
    ww_detector_init(&runner->detector, effective.sensitivity);
    if (runner->frontend == nullptr) {
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    et_model_config_t model_cfg = ET_MODEL_CONFIG_DEFAULT(pte, pte_len);
    model_cfg.arena_size = kMethodArenaBytes;
    model_cfg.arena_location = ET_MEM_DEFAULT;
    /* Planned buffers (36 KiB for Hey Muse) cannot be ET_MEM_INTERNAL:
     * in the full firmware internal RAM is nearly exhausted once Wi-Fi/BLE
     * are up (~12 KiB free, 4 KiB largest block on BOX-3), so the reload in
     * ww_runner_reset() fails with "allocating planned buffer failed" at
     * wake.on time even though create succeeded at boot. ET_MEM_DEFAULT
     * routes large blocks to PSRAM where present and falls back to internal
     * RAM on boards without it. Revisit per board if timing data says the
     * activations belong in internal RAM. */
    model_cfg.planned_location = ET_MEM_DEFAULT;
    runner->model = et_model_create(&model_cfg);
    if (runner->model == nullptr) {
        ESP_LOGE(kTag, "et_model_create failed");
        ww_frontend_destroy(runner->frontend);
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    runner->ready = et_model_load_method(runner->model, "forward");
    et_tensor_info_t input_info = {};
    et_tensor_info_t output_info = {};
    if (!runner->ready || et_model_num_inputs(runner->model) != 1 ||
        et_model_num_outputs(runner->model) != 1 ||
        !et_model_input_info(runner->model, 0, &input_info) ||
        !et_model_output_info(runner->model, 0, &output_info) || input_info.nbytes != kInputBytes ||
        output_info.nbytes != kOutputBytes) {
        ESP_LOGE(kTag, "model contract mismatch: %s", et_model_last_error(runner->model));
        et_model_destroy(runner->model);
        ww_frontend_destroy(runner->frontend);
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    size_t planned_count = 0;
    size_t planned_bytes = 0;
    et_model_planned_info(runner->model, NULL, &planned_count, &planned_bytes);
    ESP_LOGI(kTag,
             "Hey Muse runner ready: pte=%zu B planned=%zu B in %zu buffer(s) "
             "arena=%zu B",
             pte_len, planned_bytes, planned_count, kMethodArenaBytes);
    return runner;
}

void ww_runner_destroy(ww_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    et_model_destroy(runner->model);
    ww_frontend_destroy(runner->frontend);
    free(runner->pte_copy);
    delete runner;
}

void ww_runner_reset(ww_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    ww_runner_et_reset_locked(runner, true);
}

ww_event_t ww_runner_feed(ww_runner_t *runner, const int16_t *pcm, size_t frames)
{
    if (runner == nullptr || pcm == nullptr) {
        return WW_EVENT_NONE;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (!runner->ready || !runner->listening || runner->suppressed) {
        return WW_EVENT_NONE;
    }

    runner->fired = false;
    const int64_t push_t0 = esp_timer_get_time();
    ww_frontend_push(runner->frontend, pcm, frames, ww_runner_et_frame_cb, runner);
    /* Includes the per-frame inference the callback runs; frontend-only
     * cost is this accumulated push time minus infer_us. */
    runner->push_us += (uint64_t)(esp_timer_get_time() - push_t0);
    return runner->fired ? WW_EVENT_WAKE : WW_EVENT_NONE;
}

void ww_runner_set_listening(ww_runner_t *runner, bool on)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (runner->listening != on) {
        runner->listening = on;
        if (!on) {
            ww_runner_et_reset_locked(runner, false);
        }
    }
}

void ww_runner_set_suppressed(ww_runner_t *runner, bool on)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (runner->suppressed != on) {
        runner->suppressed = on;
        if (on) {
            ww_runner_et_reset_locked(runner, false);
        }
    }
}

const char *ww_runner_kind(void)
{
    return "executorch";
}

} // extern "C"
