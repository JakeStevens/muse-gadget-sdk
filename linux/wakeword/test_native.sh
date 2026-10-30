#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$root/build-signal"}
mkdir -p "$out"
cc=${CC:-cc}
sh "$root/build_signal.sh" "$out"
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root" \
    "$root/tests/signal_test.c" "$root/ww_frontend.c" "$root/ww_detector.c" \
    -L"$out" -lmuse_wakeword_signal -Wl,-rpath,"\$ORIGIN" -lm -o "$out/signal_test"
"$out/signal_test"
printf 'Built and tested %s\n' "$out/libmuse_wakeword_signal.so"
