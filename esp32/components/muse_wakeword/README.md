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

Hands-free activation: saying "Hey Muse" (or "Ok Muse") starts the
same voice note as holding the talk button. The design is
`esp32-design.md` in the program workspace; section numbers below
refer to it.

## Status: ExecuTorch runner implemented; feature still off by default

Everything on the SDK side is implemented: the §5.1 audio tap, the
wake task, synthetic-PTT turns with seeded pre-roll, playback/PTT
suppression, the resting vote, NVS opt-in, and the
`enable_wakeword`/`disable_wakeword` gadget commands. Behind the
runner contract (`include/ww_runner.h`), picked by
`MUSE_WAKEWORD_RUNNER`:

- `ww_runner_et.cpp` (default) — the production runner: the SDK-owned
  LFBE frontend (`ww_frontend.c`) and posterior detector
  (`ww_detector.c`) driving the embedded `models/hey_muse.pte`
  through the generated ExecuTorch component. Endpointing after a
  wake is energy-based; there is no VAD model. The shipped `.pte` is
  the int8 export (ESP-NN PTQ, fully delegated) and needs the
  component's `wakeword_espnn` op profile
  (`CONFIG_ET_OP_PROFILE_WAKEWORD_ESPNN=y`); on-device frame time is
  unmeasured — the previous Cadence-backend export measured ~12–16 ms
  per 20 ms frame (frontend included ~18 ms). (A portable-fp32 variant
  measured ~69 ms/frame and was removed.)
- `ww_runner_bench.c` — on-device triggers for bring-up: the serial
  bench keys below, or a loud-then-quiet energy burst (a clap). No
  speech detection of any kind. No board overlay selects it.

There is no fake runner: host tests script `ww_event_t` sequences
directly in `tests/muse_wakeword_harness.c`. The boot log and every
command reply name the linked runner, so a bench build cannot be
mistaken for detection.

The current `.pte` is a synthetic-data research checkpoint with a
high false-alarm rate (see `models/README.md`); it proves the
pipeline, it is not a production-tuned detector.

Also here: `include/muse_wakeword.h` + `muse_wakeword_core.c` —
the §4 session state machine with every §4.1 timer, the §4.1
integer energy fallback, and the caller-owned frame ring behind the
§5.2 handoff and §5.3 pre-roll. The firmware glue lives in
`components/muse/muse_wakeword_glue.{c,h}` (the voice loop calls
into it; components cannot require each other). `MUSE_WAKEWORD_ENABLE`
defaults to `n` and no board overlay sets it, so shipped builds are
unchanged.

Verified by `tests/test_muse_wakeword.py`: state machine and
timers against a Python model of §4, LFBE framing/silence/sine
behavior, detector warm-up/smoothing/cooldown, bench-runner trigger
shapes, rings, energy, plus source contracts (commands advertised
exactly where dispatched, tap inside both read loops, off by
default):

```sh
python3 -m unittest tests.test_muse_wakeword -v
```

## Trying it on a BOX-3

For the real runner, build with `CONFIG_MUSE_WAKEWORD_ENABLE=y`
(the ExecuTorch runner is the default choice and selects the runtime)
in the build's `sdkconfig` (token setup as usual), flash, run
`>wake.on`, and say "Hey Muse". Expect research-checkpoint behavior,
including false triggers; `@wake` and the `muse_wake`/`ww_et` logs
show the flow.

For plumbing-only bring-up, build instead with
`CONFIG_MUSE_WAKEWORD_RUNNER_BENCH=y`, flash with
`tools/muse/board.sh flash box3`, then over serial:

```text
>wake.on       listening on (persisted; >wake.off turns it off)
>wake=1        fires a wake; stay quiet and T_start cancels it
>wake.test=1   wake + speech; the 700 ms silence endpoint ends it
>wake          prints @wake status (also what the commands return)
```

Or clap loudly near the microphones while listening. On-device, the
same persisted opt-in is the **Sound > Wake word** settings switch
(on button boards, the quick menu's **Wake word** row); bench builds
label that switch **Clap test** instead, since clapping is all the
bench runner detects. A submit only
reaches Muse once the gadget is paired; unpaired, the turn stops
at the setup caption, and the `muse_wake` log lines plus `@wake`
counters show the flow. In the Muse app once paired,
`enable_wakeword` and `disable_wakeword` do the same as `wake.on`
and `wake.off`.

## Still to do before real enablement

- A production-quality checkpoint and detector tuning from board
  recordings. The shipped research model false-alarms far too often
  for a default-on feature (see `models/README.md`).
- The persistent §6.3 listening indicator in the UI (today: the
  ARMED caption, `@wake`, and logs).
- ADPCM pre-roll for `MUSE_LOW_MEM` boards (§5.3); today pre-roll
  is 1 s of PCM from `MUSE_BIG_CAPS`, and a failed allocation just
  disables wake word.
- `validate_config.cmake` checks from §8.2, then per-board
  enablement through the §8.4 checklist (size, on-device CPU/RAM,
  resting power with listening on, and acoustic evidence). On the
  BOX-3 the wake task stack is 8 KiB (measured peak ~2.3 KiB for the
  int8 runner) and the 64 KiB method arena + 36 KiB planned buffers
  sit in PSRAM (internal RAM cannot hold them once Wi-Fi is up);
  re-measure per board.
