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

#include <stddef.h>

#include "ww_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scripting for the fake runner (ww_runner_fake.c): each feed() call
 * returns the next scripted event, then WW_EVENT_NONE forever. Audio
 * content is ignored. The listening gate is honoured (not listening
 * always yields NONE); the suppressed flag is recorded but events are
 * deliberately still returned, so SDK-side playback suppression
 * (esp32-design.md §4.2) is what simulator tests exercise. reset()
 * rewinds the script. With no script installed the fake never wakes:
 * that is the firmware behaviour until the real runner (D2) lands.
 */
void ww_runner_fake_set_script(ww_runner_t *r, const ww_event_t *events, size_t count);

#ifdef __cplusplus
}
#endif
