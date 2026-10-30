# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

"""Export the trusted Hey Muse checkpoint to stateful XNNPACK fp32.

Checkpoint loading and architecture import follow the SDK's ESP exporter;
Cadence/ESP exports and their generated frontend constants are not modified.
Only trusted checkpoints are safe: torch.load uses weights_only=False.
Validation streams are normalized LFBE, not raw audio. They prove numerical
parity, NOT accuracy or false-alarm rates. No quantization is performed.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import re
import struct
import subprocess
import sys
import types
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import torch

EXPECTED_FRONTEND = {
    "sample_rate": 16000,
    "window_samples": 480,
    "hop_samples": 320,
    "n_fft": 512,
    "n_mels": 40,
    "fmin": 0.0,
    "fmax": 8000.0,
    "mel_scale": "slaney",
    "log_eps": 1e-6,
}
EXPECTED_CLASSES = ["wakeword", "silence", "other"]
CAVEAT = (
    "Research/synthetic-data checkpoint, opt-in only. Training bundle reports "
    "91% recall at 23.9 false alarms/hour at threshold .95 and 59% recall at "
    "8.9 false alarms/hour at .99; no threshold reaches <=1 false alarm/hour. "
    "These are not Linux accuracy measurements."
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_streaming_class(root: Path):
    # Same architecture-loading compatibility shim as the ESP export tool.
    try:
        import executorch.examples.models.model_base  # noqa: F401
    except ModuleNotFoundError:
        stub = types.ModuleType("executorch.examples.models.model_base")
        stub.EagerModelBase = object
        sys.modules[stub.__name__] = stub
    path = root / "examples/models/mlperf_tiny/streaming_wakeword.py"
    spec = importlib.util.spec_from_file_location("streaming_wakeword", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"Cannot load StreamingWakeWord from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.StreamingWakeWord


def validate_frontend(frontend: dict, header: Path) -> None:
    for key, expected in EXPECTED_FRONTEND.items():
        if frontend.get(key) != expected:
            raise ValueError(f"Unsupported frontend {key}={frontend.get(key)!r}")
    text = header.read_text()
    for key, name in (
        ("norm_mean", "kWwFrontendNormMean"),
        ("norm_std", "kWwFrontendNormStd"),
    ):
        match = re.search(rf"{name}\[40\]\s*=\s*\{{([^}}]+)\}}", text)
        if match is None:
            raise ValueError(f"Missing frozen normalization array {name}")
        values = [
            float(v.strip().removesuffix("f"))
            for v in match.group(1).split(",")
            if v.strip()
        ]
        expected = frontend[key]
        if (
            len(values) != 40
            or len(expected) != 40
            or any(
                not math.isfinite(float(b))
                or not math.isclose(a, float(b), rel_tol=1e-8)
                for a, b in zip(values, expected)
            )
        ):
            raise ValueError(f"Checkpoint {key} differs from frozen ESP frontend")
        if key == "norm_std" and any(v <= 0 for v in values):
            raise ValueError("Normalization standard deviations must be positive")


def select_streams(path: Path, clips_per_kind: int) -> tuple[torch.Tensor, list[str]]:
    import torch

    bundle = torch.load(path, map_location="cpu", weights_only=False)
    features = bundle["features"]
    metadata = bundle["metadata"]
    if features.ndim != 3 or features.shape[1] != 40 or len(metadata) != len(features):
        raise ValueError("Invalid calibration feature/metadata shapes")
    selected = []
    kinds = []
    counts = {}
    for i, record in enumerate(metadata):
        kind = record["kind"]
        if counts.get(kind, 0) < clips_per_kind:
            selected.append(i)
            kinds.append(kind)
            counts[kind] = counts.get(kind, 0) + 1
    if set(counts) != {"positive", "negative_speech", "silence"}:
        raise ValueError(f"Expected all three validation kinds, got {counts}")
    streams = features[selected].transpose(1, 2).contiguous().float()
    if not torch.isfinite(streams).all():
        raise ValueError("Nonfinite validation features")
    return streams, kinds


def eager_posteriors(model, streams: torch.Tensor) -> torch.Tensor:
    outputs = []
    import torch

    with torch.no_grad():
        for stream in streams:
            model.reset()
            outputs.append(
                torch.stack(
                    [
                        model(frame.reshape(1, 40, 1, 1)).reshape(3).clone()
                        for frame in stream
                    ]
                )
            )
    model.reset()
    return torch.stack(outputs)


def validate_runtime(pte: Path, streams: torch.Tensor, expected: torch.Tensor) -> dict:
    from executorch.runtime import Runtime

    runtime = Runtime.get()
    if not runtime.backend_registry.is_available("XnnpackBackend"):
        raise RuntimeError("Validation runtime was built without XNNPACK")
    actual = []
    reset_difference = 0.0
    import torch

    with torch.no_grad():
        for stream in streams:
            # The native reset contract recreates a method; test those exact semantics.
            program = runtime.load_program(pte)
            method = program.load_method("forward")
            rows = torch.stack(
                [
                    method.execute((frame.reshape(1, 40, 1, 1),))[0].reshape(3).clone()
                    for frame in stream
                ]
            )
            for frame in stream.flip(0):
                method.execute((frame.reshape(1, 40, 1, 1),))
            program = runtime.load_program(pte)
            method = program.load_method("forward")
            repeated = torch.stack(
                [
                    method.execute((frame.reshape(1, 40, 1, 1),))[0].reshape(3).clone()
                    for frame in stream
                ]
            )
            reset_difference = max(
                reset_difference, float((rows - repeated).abs().max())
            )
            actual.append(rows)
    actual = torch.stack(actual)
    torch.testing.assert_close(actual, expected, rtol=1e-4, atol=1e-5)
    if reset_difference != 0.0:
        raise AssertionError(f"Fresh/reset stream mismatch: {reset_difference}")
    difference = (actual - expected).abs()
    return {
        "clips": len(streams),
        "frames": streams.shape[0] * streams.shape[1],
        "includes_warmup": True,
        "max_abs_error": float(difference.max()),
        "mean_abs_error": float(difference.mean()),
        "reset_max_abs_error": reset_difference,
        "rtol": 1e-4,
        "atol": 1e-5,
        "runtime": "ExecuTorch Python XNNPACK",
        "status": "passed",
    }


def write_floats(path: Path, tensor: torch.Tensor) -> None:
    flat = tensor.reshape(-1).tolist()
    with path.open("wb") as output:
        for start in range(0, len(flat), 4096):
            chunk = flat[start : start + 4096]
            output.write(struct.pack(f"<{len(chunk)}f", *chunk))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--calibration", type=Path, required=True)
    parser.add_argument("--executorch-root", type=Path, required=True)
    parser.add_argument("--frontend-header", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--parity-dir", type=Path, required=True)
    parser.add_argument("--clips-per-kind", type=int, default=8)
    parser.add_argument(
        "--defer-runtime-validation",
        action="store_true",
        help="Emit native parity fixtures and mark validation pending; run verify_parity.py before shipping",
    )
    args = parser.parse_args()
    if args.clips_per_kind < 1:
        parser.error("--clips-per-kind must be positive")
    import torch

    torch.set_num_threads(1)
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    config = checkpoint["config"]
    validate_frontend(config["frontend"], args.frontend_header)
    if config["model"].get("class_names") != EXPECTED_CLASSES:
        raise ValueError("Unexpected class order")
    streaming_cls = load_streaming_class(args.executorch_root)
    model = streaming_cls(apply_softmax=True).eval()
    missing, unexpected = model.load_state_dict(
        checkpoint["model_state_dict"], strict=False
    )
    if unexpected or any(not key.endswith(".history") for key in missing):
        raise ValueError(
            f"Checkpoint mismatch: missing={missing}, unexpected={unexpected}"
        )
    model.reset()
    streams, kinds = select_streams(args.calibration, args.clips_per_kind)
    expected = eager_posteriors(model, streams)

    from executorch.backends.xnnpack.partition.xnnpack_partitioner import (
        XnnpackFloatingPointPartitioner,
    )
    from executorch.exir import ExecutorchBackendConfig, to_edge_transform_and_lower
    from executorch.exir.passes.init_mutable_pass import InitializedMutableBufferPass

    with torch.no_grad():
        exported = torch.export.export(
            model, (torch.zeros(streaming_cls.FRAME_SHAPE),), strict=True
        )
        # torch 2.14 preserves copy_ until functionalization exposes mutations.
        exported = exported.run_decompositions()
        mutations = list(exported.graph_signature.buffers_to_mutate.values())
        if set(mutations) != set(streaming_cls.HISTORY_NAMES):
            raise ValueError(f"History mutations lost in export: {mutations}")
        edge = to_edge_transform_and_lower(
            exported, partitioner=[XnnpackFloatingPointPartitioner()]
        )
        model.reset()
        program = edge.to_executorch(
            config=ExecutorchBackendConfig(
                passes=[InitializedMutableBufferPass(["history"])],
            )
        )
    plan = program.executorch_program.execution_plan[0]
    delegates = [delegate.id for delegate in plan.delegates]
    if not delegates or set(delegates) != {"XnnpackBackend"}:
        raise ValueError(f"Expected real XNNPACK delegates: {delegates}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    pte = args.output_dir / "hey_muse_xnnpack.pte"
    with pte.open("wb") as output:
        program.write_to_file(output)
    parity = (
        {
            "status": "pending",
            "required": "native verify_parity.py sequential/reset validation",
        }
        if args.defer_runtime_validation
        else validate_runtime(pte, streams, expected)
    )
    args.parity_dir.mkdir(parents=True, exist_ok=True)
    write_floats(args.parity_dir / "frames.f32", streams)
    write_floats(args.parity_dir / "expected.f32", expected)
    manifest = {
        "clips": len(streams),
        "frames_per_clip": streams.shape[1],
        "features_per_frame": 40,
        "posteriors_per_frame": 3,
        "kinds": kinds,
        "encoding": "little-endian float32",
    }
    (args.parity_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    import executorch.exir as exir
    import executorch.version as et_version

    architecture = (
        args.executorch_root / "examples/models/mlperf_tiny/streaming_wakeword.py"
    )
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(args.executorch_root), "rev-parse", "HEAD"], text=True
        ).strip()
    except subprocess.CalledProcessError:
        commit = None
    sidecar = {
        "schema_version": 1,
        "model": "StreamingWakeWord",
        "backend": "XNNPACK",
        "precision": "fp32",
        "quantization": "none",
        "experimental": True,
        "source": "manifold://executorch/tree/jrstevens/hey-muse",
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "checkpoint_epoch": checkpoint.get("epoch"),
        "pte_sha256": sha256_file(pte),
        "pte_size": pte.stat().st_size,
        "architecture_sha256": sha256_file(architecture),
        "architecture_commit": commit,
        "exporter_git_version": et_version.git_version,
        "exporter_exir": str(Path(exir.__file__).resolve()),
        "torch_version": torch.__version__,
        "class_names": EXPECTED_CLASSES,
        "wake_phrases": config.get("data", {}).get("wake_phrases", []),
        "frontend": config["frontend"],
        "frontend_header_sha256": sha256_file(args.frontend_header),
        "input_shape": list(streaming_cls.FRAME_SHAPE),
        "output_shape": [1, 3],
        "method": "forward",
        "mutable_histories": mutations,
        "histories_initialized": "zeros",
        "reset": "fresh wheel program/method",
        "delegates": delegates,
        "portable_ops": [op.name + "." + op.overload for op in plan.operators],
        "validation_data_sha256": sha256_file(args.calibration),
        "runtime_vs_eager": parity,
        "notes": CAVEAT,
        "detector": {
            "thresholds": {"low": 0.99, "default": 0.95, "high": 0.90},
            "warmup_frames": 29,
            "smooth_frames": 5,
            "cooldown_frames": 50,
        },
    }
    (args.output_dir / "hey_muse_xnnpack.json").write_text(
        json.dumps(sidecar, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps({"pte": str(pte), "size": pte.stat().st_size, "parity": parity}))


if __name__ == "__main__":
    main()
