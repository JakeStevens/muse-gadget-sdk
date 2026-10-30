# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

import hashlib
import json
import sys
from pathlib import Path
from types import SimpleNamespace

from musegadget.wakeword import _WheelModel

SIGNAL = Path(__file__).resolve().parents[1] / "wakeword"


def test_copied_signal_sources_match_updated_esp32_snapshot():
    manifest = json.loads((SIGNAL / "esp32_sources.json").read_text())
    assert manifest["schema_version"] == 1
    assert manifest["source_commit"] == "0ae58f014d75737e3fc6506271beca108a7b567a"
    assert set(manifest["files"]) == {
        "ww_frontend.c",
        "ww_frontend.h",
        "ww_frontend_config.h",
        "ww_detector.c",
        "ww_detector.h",
        "ww_runner.h",
    }
    for filename, expected in manifest["files"].items():
        assert hashlib.sha256((SIGNAL / filename).read_bytes()).hexdigest() == expected


def test_wheel_loader_keeps_model_file_ownership_in_runtime(tmp_path, monkeypatch):
    # ESP embeds aligned flash bytes; Linux must continue to use the file API.
    path = tmp_path / "model with spaces.pte"
    path.write_bytes(b"synthetic model for public-loader contract tests")
    loaded = []
    methods = []

    def tensor_info(shape):
        return SimpleNamespace(sizes=lambda: shape, dtype=lambda: 6)

    meta = SimpleNamespace(
        num_inputs=lambda: 1,
        num_outputs=lambda: 1,
        input_tensor_meta=lambda index: tensor_info((1, 40, 1, 1)),
        output_tensor_meta=lambda index: tensor_info((1, 3)),
    )

    def load_program(argument):
        assert isinstance(argument, Path) and argument == path
        loaded.append(argument)
        method = SimpleNamespace(metadata=meta)
        methods.append(method)
        return SimpleNamespace(
            method_names={"forward"}, load_method=lambda name: method
        )

    runtime = SimpleNamespace(
        backend_registry=SimpleNamespace(
            is_available=lambda name: name == "XnnpackBackend"
        ),
        load_program=load_program,
    )
    monkeypatch.setitem(sys.modules, "torch", SimpleNamespace())
    monkeypatch.setitem(
        sys.modules,
        "executorch.runtime",
        SimpleNamespace(Runtime=SimpleNamespace(get=lambda: runtime)),
    )
    model = _WheelModel(path)
    try:
        assert model._method is methods[0]
        model.reset()
        assert loaded == [path, path]
        assert model._method is methods[1] and methods[0] is not methods[1]
    finally:
        model.close()
    assert model._method is None and model._program is None
