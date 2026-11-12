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

#ifdef __cplusplus
extern "C" {
#endif

/* Confidence threshold for pushing, in the 0-100 class percent. */
#define WATCHER_CONFIDENCE_THRESHOLD 75
/* Watch loop period: one capture + inference attempt per interval. */
#define WATCHER_WATCH_PERIOD_MS 2000
/* ImageNet class index for "coffee mug" (torchvision ordering).
 * Only this target fires; every other class is ignored. */
#define WATCHER_MUG_TARGET 504

/*
 * Arm or disarm coffee mug watching. Starts the watch task on the first
 * call. Safe to call from the command handler task. Returns false
 * only if the lock or the watch task could not be created.
 */
bool watcher_mug_set_armed(bool armed);

/* True when coffee mug watching is armed. */
bool watcher_mug_is_armed(void);

#ifdef __cplusplus
}
#endif
