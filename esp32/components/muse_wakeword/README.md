<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# muse_wakeword

Hands-free activation: saying "Marvin" starts the same voice note as
holding the talk button. The design is `esp32-design.md` in the
program workspace; section numbers below refer to it.

## Status: M1 plumbing against mock runners (no detection yet)

Everything on the SDK side is implemented: the §5.1 audio tap, the
wake task, synthetic-PTT turns with seeded pre-roll, playback/PTT
suppression, the resting vote, NVS opt-in, and the
`enable_wakeword`/`disable_wakeword` gadget commands. What sits
behind the §3.2 runner contract (`include/ww_runner.h`) is still a
test double, picked by `MUSE_WAKEWORD_RUNNER`:

- `ww_runner_fake.c` (default) — scripted in host tests; on a device
  it **cannot detect anything**.
- `ww_runner_bench.c` — on-device triggers for bring-up: the serial
  bench keys below, or a loud-then-quiet energy burst (a clap). No
  speech detection of any kind. No board overlay selects it.

The boot log and every command reply name the runner, so a mock
build cannot be mistaken for detection. When D1–D3 land (§9.2), the
ExecuTorch runner joins the Kconfig choice, `EMBED_FILES
models/marvin.pte` is added here, and the mocks go away; nothing in
the glue changes.

Also here: `include/muse_wakeword.h` + `muse_wakeword_core.c` —
the §4 session state machine with every §4.1 timer, the §4.1
integer energy fallback, and the caller-owned frame ring behind the
§5.2 handoff and §5.3 pre-roll. The firmware glue lives in
`components/muse/muse_wakeword_glue.{c,h}` (the voice loop calls
into it; components cannot require each other). `MUSE_WAKEWORD_ENABLE`
defaults to `n` and no board overlay sets it, so shipped builds are
unchanged; the Silero VAD tier and `MUSE_WAKEWORD_MODEL_PATH`
appear only when they can actually work (§8.1).

Verified by `tests/test_muse_wakeword.py`: state machine and
timers against a Python model of §4, bench-runner trigger shapes,
rings, energy, plus source contracts (commands advertised exactly
where dispatched, tap inside both read loops, off by default):

```sh
python3 -m unittest tests.test_muse_wakeword -v
```

## Trying it on a BOX-3

Build with `CONFIG_MUSE_WAKEWORD_ENABLE=y` and
`CONFIG_MUSE_WAKEWORD_RUNNER_BENCH=y` in the build's `sdkconfig`
(token setup as usual), flash with `tools/muse/board.sh flash
box3`, then over serial:

```text
>wake.on       listening on (persisted; >wake.off turns it off)
>wake=1        fires a wake; stay quiet and T_start cancels it
>wake.test=1   wake + speech; the 700 ms silence endpoint ends it
>wake          prints @wake status (also what the commands return)
```

Or clap loudly near the microphones while listening. Bench builds also
have a temporary **Sound > Clap test** settings switch for the same
persisted opt-in; it disappears outside bench-runner builds. A submit only
reaches Muse once the gadget is paired; unpaired, the turn stops
at the setup caption, and the `muse_wake` log lines plus `@wake`
counters show the flow. In the Muse app once paired,
`enable_wakeword` and `disable_wakeword` do the same as `wake.on`
and `wake.off`.

## Still to do before real enablement

- The persistent §6.3 listening indicator in the UI (today: the
  ARMED caption, `@wake`, and logs; acceptable while only bench
  builds can hear anything, not for shipping detection).
- ADPCM pre-roll for `MUSE_LOW_MEM` boards (§5.3); today pre-roll
  is 1 s of PCM from `MUSE_BIG_CAPS`, and a failed allocation just
  disables wake word.
- `validate_config.cmake` checks from §8.2, then per-board
  enablement through the §8.4 checklist (size, on-device CPU/RAM,
  resting power with listening on, and acoustic evidence).
- Wake task stack/CPU numbers are placeholders sized for the mocks;
  D4 sets the real ones (§10.2).
