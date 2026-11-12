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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A still camera, whichever one the board has. Each camera is a backend that
 * fills in a camera_driver_t, and the board registers its one at startup.
 * Callers don't see the backend: they take a JPEG with camera_capture() and
 * hand it back with camera_release(), or watch a stream of frames for a
 * viewfinder with camera_stream_start() and camera_stream_stop().
 */

/* Classification results, best first, as reported by the Himax
 * WiseEye2 on an INVOKE event: [score, target] pairs with score 0-100.
 * Empty (n_classes == 0) for SAMPLE captures, which carry no classes. */
#define CAMERA_MAX_CLASSES 5

typedef struct {
    const uint8_t *jpeg;
    size_t len;
    int width, height;
    void *priv;               /* the backend's, for release */
    int n_classes;
    int class_target[CAMERA_MAX_CLASSES];
    int class_score[CAMERA_MAX_CLASSES];
} camera_frame_t;

/* A streamed frame, on the backend's task; it's freed when this returns. */
typedef void (*camera_frame_cb_t)(const camera_frame_t *frame, void *ctx);

typedef struct {
    const char *name;         /* for people and Muse: "Himax WiseEye2 (SenseCAP Watcher)" */
    int max_width, max_height;
    /* Once, before the first capture; NULL if there's nothing to set up. */
    esp_err_t (*init)(void);
    /* One JPEG. Powers the camera only as long as it needs to. */
    esp_err_t (*capture)(camera_frame_t *out);
    /* One JPEG plus classification results (n_classes filled). Falls
     * back to capture() with n_classes == 0 when NULL. Powers the
     * camera only as long as it needs to. */
    esp_err_t (*classify)(camera_frame_t *out);
    /* Frees what capture() gave out; NULL if nothing needs freeing. */
    void (*release)(camera_frame_t *frame);
    /* Holds the camera powered across captures: power_hold(true) boots
     * it once; later captures run warm. power_hold(false) powers it
     * down. NULL when the backend can't hold power. */
    esp_err_t (*power_hold)(bool on);
    /* Frames until stream_stop(), smaller ones to keep up; the camera stays
     * powered meanwhile. Both NULL if it can't stream. */
    esp_err_t (*stream_start)(camera_frame_cb_t on_frame, void *ctx);
    void (*stream_stop)(void);
} camera_driver_t;

/* The board's camera; call before the first capture. */
void camera_register(const camera_driver_t *driver);

/* The registered camera, or NULL: this device has none. */
const camera_driver_t *camera_get(void);

/*
 * Takes one picture. Starts the backend on first use. One frame is out at a
 * time: until camera_release(), another capture returns ESP_ERR_INVALID_STATE.
 * ESP_ERR_NOT_SUPPORTED if there's no camera.
 */
esp_err_t camera_capture(camera_frame_t *out);
void camera_release(camera_frame_t *frame);

/*
 * Takes one picture with classification results (n_classes filled when
 * the backend supports it). Same one-frame-at-a-time rule as
 * camera_capture(); ESP_ERR_NOT_SUPPORTED if there's no camera.
 */
esp_err_t camera_classify(camera_frame_t *out);

/*
 * Holds the camera powered across captures (watch loop armed) or
 * powers it down (disarmed). ESP_ERR_NOT_SUPPORTED when the backend
 * has no power_hold op. Call only when no frame is held and no
 * stream is running.
 */
esp_err_t camera_power_hold(bool on);

/*
 * Calls on_frame with each frame until camera_stream_stop(), which returns
 * once no more will come. A stream holds the camera as a frame does: capture
 * and another stream return ESP_ERR_INVALID_STATE meanwhile. Starts the
 * backend on first use. ESP_ERR_NOT_SUPPORTED if it can't stream.
 */
esp_err_t camera_stream_start(camera_frame_cb_t on_frame, void *ctx);
void camera_stream_stop(void);

#ifdef __cplusplus
}
#endif
