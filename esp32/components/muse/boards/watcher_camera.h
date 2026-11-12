/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <stdbool.h>
#include "camera.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Double-click to open live preview, then tap the shutter or double-click again. */
void watcher_camera_preview_toggle(void);
bool watcher_camera_preview_active(void);
/* Captures one frame for the Home Link camera.capture command. */
bool watcher_camera_capture(char **jpeg_base64, const char **error);
/* Classifies one frame: JPEG plus data.classes (n_classes filled) via
 * AT+INVOKE. The caller releases the frame with camera_release().
 * False with *error set when the camera is busy or unresponsive. */
bool watcher_camera_classify(camera_frame_t *out, const char **error);
/* Releases a frame from watcher_camera_classify(). Must be called
 * exactly once for every successful classify. */
void watcher_camera_release(camera_frame_t *frame);
/* Holds the camera powered across captures (true) or powers it down
 * (false). Call only when no frame is held and the preview is off. */
esp_err_t watcher_camera_set_power_hold(bool on);
esp_err_t watcher_camera_prepare(void);

#ifdef __cplusplus
}
#endif
