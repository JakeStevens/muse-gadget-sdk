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
#include "ww_runner_fake.h"

#include <stdlib.h>

/*
 * Host-test double for the ww_runner contract: scripted events for the
 * simulator and unit tests. Production firmware links either the
 * ExecuTorch runner or the bench runner instead. With no script
 * installed this test double cannot wake.
 */
struct ww_runner {
    ww_config_t cfg;
    const ww_event_t *script;
    size_t script_len;
    size_t script_at;
    bool listening;
    bool suppressed;   /* recorded only; see ww_runner_fake.h */
};

ww_runner_t *ww_runner_create(const ww_config_t *cfg)
{
    ww_runner_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    if (cfg) {
        r->cfg = *cfg;
    } else {
        r->cfg.sample_rate_hz = 16000;
        r->cfg.sensitivity = WW_SENSITIVITY_DEFAULT;
        r->cfg.vad = WW_VAD_ENERGY;
    }
    r->listening = true;
    return r;
}

void ww_runner_destroy(ww_runner_t *r)
{
    free(r);
}

void ww_runner_reset(ww_runner_t *r)
{
    if (r) {
        r->script_at = 0;
    }
}

ww_event_t ww_runner_feed(ww_runner_t *r, const int16_t *pcm, size_t frames)
{
    (void)pcm;
    (void)frames;
    if (!r) {
        return WW_EVENT_NONE;
    }
    ww_event_t ev = r->script_at < r->script_len ? r->script[r->script_at] : WW_EVENT_NONE;
    r->script_at++;   /* audio time passes whether or not anyone listens */
    return r->listening ? ev : WW_EVENT_NONE;
}

void ww_runner_set_listening(ww_runner_t *r, bool on)
{
    if (r) {
        r->listening = on;
    }
}

void ww_runner_set_suppressed(ww_runner_t *r, bool on)
{
    if (r) {
        r->suppressed = on;
    }
}

void ww_runner_fake_set_script(ww_runner_t *r, const ww_event_t *events, size_t count)
{
    if (!r) {
        return;
    }
    r->script = events;
    r->script_len = events ? count : 0;
    r->script_at = 0;
}
