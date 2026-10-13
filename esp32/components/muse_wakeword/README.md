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

## Status: M0 (integration core, no detection yet)

This component is the SDK-owned side that does not need the other
workstream's deliverables:

- `include/ww_runner.h` - the §3.2 interface contract with the wake
  word runner / ExecuTorch backend / model. No ESP-IDF types, so the
  host tests link the same code.
- `ww_runner_fake.c` - a scripted stand-in implementing that contract
  for the simulator. With no script installed, as in firmware, it
  **cannot detect anything**.
- `include/muse_wakeword.h` + `muse_wakeword_core.c` - the §4
  session state machine, the §4.1 integer energy fallback, and the
  caller-owned frame ring behind the §5.2 handoff and §5.3 pre-roll.
- `Kconfig` - the §8.1 options. `MUSE_WAKEWORD_ENABLE` defaults to
  `n` and no board overlay sets it, so shipped builds are unchanged.

Run the host tests with:

```sh
python3 -m unittest tests.test_muse_wakeword -v
```

The firmware glue (audio tap, wake task, synthetic turns, opt-in,
and gadget commands) lands in follow-up commits. Real detection
arrives with the ExecuTorch component, runner, and model (§3.1
D1-D3), together per §9.2.
