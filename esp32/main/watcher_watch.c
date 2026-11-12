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

/*
 * Watcher watch loop: while armed, run ImageNet classification on
 * one frame every 2 seconds with the Himax (AT+INVOKE) and, when
 * the top result is a coffee mug (class 504) above
 * WATCHER_CONFIDENCE_THRESHOLD, push the result plus the image
 * to Muse as a typed chat turn. Coffee mug only: no other class
 * triggers a push, so a laptop on the desk never fires it. Nothing
 * is pushed while disarmed, while a voice turn runs, or while the
 * VM is unreachable (the push refuses and the loop retries next
 * cycle). Every classified cycle logs the mug percent, so a
 * threshold that never fires is visible.
 * Power follows arming: arming powers the camera on once and it
 * stays booted across cycles (warm classifies, no cold boot per
 * frame); disarming powers it down. If holding power fails, cycles
 * fall back to powering per frame until the camera boots. Capture
 * may overrun the period; the loop then starts the next cycle
 * immediately rather than stacking up.
 */

#include "watcher_watch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boards/watcher_camera.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_chat.h"

static const char *TAG = "watcher_watch";

static bool s_mug_armed = false;
static SemaphoreHandle_t s_lock;
static bool s_task_started;

static void watcher_watch_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    bool held = false;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(WATCHER_WATCH_PERIOD_MS));
        bool armed;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        armed = s_mug_armed;
        xSemaphoreGive(s_lock);
        if (armed && !held) {
            /* Arm powers the camera on once; cycles below stay warm.
             * INVALID_STATE (camera busy) retries silently next cycle. */
            esp_err_t herr = watcher_camera_set_power_hold(true);
            if (herr == ESP_ERR_INVALID_STATE) {
                /* retry next cycle */
            } else if (herr == ESP_ERR_NOT_SUPPORTED) {
                held = true; /* sentinel: stop trying, power per cycle */
                ESP_LOGW(TAG, "camera hold unsupported; powering per cycle");
            } else {
                held = true;
                if (herr == ESP_OK) {
                    ESP_LOGI(TAG, "camera powered on (held while armed)");
                } else {
                    ESP_LOGW(TAG, "camera hold failed; powering per cycle until it boots");
                }
            }
        } else if (!armed && held && !watcher_camera_preview_active()) {
            /* Never cut power under a running preview: defer until it stops. */
            if (watcher_camera_set_power_hold(false) != ESP_ERR_INVALID_STATE) {
                held = false;
                ESP_LOGI(TAG, "camera powered down");
            }
        }
        if (!armed) {
            continue;
        }
        const char *error = NULL;
        camera_frame_t frame;
        if (!watcher_camera_classify(&frame, &error)) {
            ESP_LOGW(TAG, "classify failed: %s", error ? error : "unknown");
            continue;
        }
        /* Coffee mug only: the Himax reports class targets
         * best-first. Fire only when the mug (target 504) is the
         * top entry above threshold; any other top result,
         * including a laptop, reports mug=0 and never fires. */
        int mug_percent = 0;
        if (frame.n_classes > 0 && frame.class_target[0] == WATCHER_MUG_TARGET) {
            mug_percent = frame.class_score[0];
        }
        ESP_LOGI(TAG, "scores: mug=%d | armed=mug", mug_percent);
        if (mug_percent > WATCHER_CONFIDENCE_THRESHOLD) {
            ESP_LOGI(TAG, "match: coffee mug %d%%, pushing image to Muse", mug_percent);
            if (!muse_chat_send_detection(frame.jpeg, frame.len, "coffee mug", mug_percent)) {
                ESP_LOGW(TAG, "push skipped (turn running or unreachable)");
            }
        }
        watcher_camera_release(&frame);
    }
}

bool watcher_mug_set_armed(bool armed)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return false;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mug_armed = armed;
    xSemaphoreGive(s_lock);
    if (!s_task_started) {
        if (xTaskCreateWithCaps(watcher_watch_task, "watcher_watch", 8192, NULL, 4, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            return false;
        }
        s_task_started = true;
    }
    ESP_LOGI(TAG, "mug watching: %s", armed ? "armed" : "disarmed");
    return true;
}

bool watcher_mug_is_armed(void)
{
    return s_mug_armed;
}
