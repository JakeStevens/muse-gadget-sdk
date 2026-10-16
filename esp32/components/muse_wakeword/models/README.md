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

# Hey Muse wake word model

`hey_muse.pte` is the int8 ExecuTorch export embedded by
`ww_runner_et.cpp` (Cadence-backend PTQ; runs on the component's
`wakeword_int8` profile — Cadence generic/portable kernels).
`hey_muse.json` is its generated provenance sidecar (checkpoint and
PTE hashes, frontend config, class order, model ops, the vendored
runtime's op-set hash, and the int8 calibration/parity record). The
runner does not read the JSON on device — the matching frontend
constants are generated into `../ww_frontend_config.h` by the same
export. The export is int8-only: a portable-fp32 variant measured
~69 ms per 20 ms frame on ESP32-S3 (3.5x over budget) and was
removed; the sidecar's parity block records int8 against the eager
fp32 model instead.

- Architecture: ExecuTorch `StreamingWakeWord`
  (`examples/models/mlperf_tiny/streaming_wakeword.py`), one LFBE
  frame in, posterior over `wakeword | silence | other` out.
- Wake phrases: "Hey Muse", "Ok Muse", "Okay Muse" (see the
  sidecar for the exact training variants).
- Frontend (frozen; `ww_frontend.c` implements exactly this):
  16 kHz PCM, 480-sample periodic Hann window centered in a
  512-point FFT, 320-sample hop, power spectrum, 40 Slaney mel
  filters with Slaney area normalization, `log(mel + 1e-6)`, then
  the checkpoint's per-mel mean/std normalization.
- No VAD. Endpointing after a wake is the SDK-side energy tier.

## Quality status: research checkpoint, not production

The checkpoint comes from
`manifold://executorch/tree/jrstevens/hey-muse` (see that bundle's
README for training data, metrics, and limitations). It was trained
only on synthetic TTS voices plus Speech Commands negatives. Its
own evaluation reports roughly 90–95% recall with on the order of
200 false alarms/hour on noisy held-out voices at threshold 0.95.
**Do not treat successful firing/non-firing on a desk as accuracy
evidence.** A production checkpoint retrained/tuned on board
recordings is required before any board enables this by default.

## Regenerating

Download the bundle, then run the export against an ExecuTorch
checkout with a matching installed Python package (see
`docs/executorch.md` for the toolchain requirements):

```sh
manifold getr executorch/tree/jrstevens/hey-muse /tmp/hey-muse --skip_root_dir
<executorch-python> components/muse_wakeword/tools/export_hey_muse.py \
  --checkpoint /tmp/hey-muse/hey_muse_checkpoint.pt \
  --executorch-root <executorch-checkout> \
  --calibration /tmp/hey-muse/calibration_frames.pt
```

The export refuses a checkpoint whose frontend/class config differs
from the frozen frontend, verifies the model's ops against
`components/executorch/VERSION`, and rewrites the `.pte`, the JSON
sidecar, and `ww_frontend_config.h` together. Review all three in
one change; a new checkpoint with different normalization vectors
is only correct with its regenerated header.
