# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The wake word session core (esp32-design.md §4/§5/§11.1): state machine,
timers, PTT/playback interleavings, audio rings, and the energy fallback,
driven through the scripted fake runner (§3.2) and checked against a Python
model written from the design rules, not from the C."""

import os
import shlex
import subprocess
import tempfile
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WW = ROOT / 'components/muse_wakeword'
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

IDLE, ARMED, CAPTURING, CAPTURING_PTT = 0, 1, 2, 3
POST_DOWN, POST_UP, CANCELLED, SUBMITTED, CONVERTED = 1, 2, 4, 8, 16
NONE, WAKE, SPEECH_START, SPEECH_END = 0, 1, 2, 3
EV = {'.': NONE, 'W': WAKE, 's': SPEECH_START, 'e': SPEECH_END}


def model(ticks, start, end, cap, energy):
    """§4 restated: wake arms only; speech opens the turn (POST_DOWN);
    T_start cancels with nothing posted; trailing silence or the hard cap
    posts up; playback discards wakes; a real press cancels arming and
    converts a capture, whose own release then ends it."""
    state, armed, capture, silence = IDLE, 0, 0, 0
    stats = [0, 0, 0, 0, 0]   # wakes, cancels, submits, conversions, suppressed
    trace = []
    for ev_ch, voiced, down, up, playback, enabled in ticks:
        ev = EV[ev_ch] if enabled else NONE   # harness: listening == enabled
        act = 0
        if not enabled:
            if state == CAPTURING:
                act = POST_UP | SUBMITTED
                stats[2] += 1
            elif state == ARMED:
                act = CANCELLED
                stats[1] += 1
            elif state == CAPTURING_PTT:
                act = SUBMITTED
                stats[2] += 1
            state, armed, capture, silence = IDLE, 0, 0, 0
            trace.append((state, act))
            continue
        if playback:
            if ev == WAKE:
                stats[4] += 1
            trace.append((state, 0))
            continue
        if down:
            if state == ARMED:
                state, act = IDLE, CANCELLED
                stats[1] += 1
            elif state == CAPTURING:
                state, act = CAPTURING_PTT, CONVERTED
                stats[3] += 1
            trace.append((state, act))
            continue
        if state == CAPTURING_PTT:
            capture += 1
            if up or capture >= cap:
                state, act = IDLE, SUBMITTED
                stats[2] += 1
        elif state == IDLE:
            if ev == WAKE:
                state, armed = ARMED, 0
                stats[0] += 1
        elif state == ARMED:
            if ev == SPEECH_START or (energy and voiced):
                state, capture, silence, act = CAPTURING, 0, 0, POST_DOWN
            else:
                armed += 1
                if armed >= start:
                    state, act = IDLE, CANCELLED
                    stats[1] += 1
        elif state == CAPTURING:
            capture += 1
            silence = 0 if ev == SPEECH_START or (energy and voiced) else silence + 1
            if ev == SPEECH_END or (energy and silence >= end) or capture >= cap:
                state, act = IDLE, POST_UP | SUBMITTED
                stats[2] += 1
        trace.append((state, act))
    return trace, stats


def tick(ev='.', voiced=0, down=0, up=0, playback=0, enabled=1):
    return (ev, voiced, down, up, playback, enabled)


class WakeWordCore(unittest.TestCase):
    START, END, CAP = 5, 3, 10

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'muse_wakeword_harness'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(WW / 'include'),
             str(ROOT / 'tests/muse_wakeword_harness.c'),
             str(WW / 'muse_wakeword_core.c'), str(WW / 'ww_runner_fake.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_core(self, ticks, energy=1):
        data = ''.join(f'{e}{v}{d}{u}{p}{n}\n' for e, v, d, u, p, n in ticks)
        ran = subprocess.run([str(self.binary), 'core', str(self.START), str(self.END),
                              str(self.CAP), str(energy)],
                             input=data.encode(), capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        lines = ran.stdout.decode().splitlines()
        trace = [tuple(map(int, line.split())) for line in lines[:-1]]
        stats = list(map(int, lines[-1].split()[1:]))
        want_trace, want_stats = model(ticks, self.START, self.END, self.CAP, energy)
        self.assertEqual(trace, want_trace)
        self.assertEqual(stats, want_stats)
        return trace

    def test_false_trigger_cancels_without_opening_a_turn(self):
        trace = self.run_core([tick('W')] + [tick()] * 8)
        # §7.1: no POST_DOWN on wake, and a cancel posts nothing at all.
        self.assertFalse(any(a & POST_DOWN for _, a in trace))
        self.assertFalse(any(a & POST_UP for _, a in trace))
        self.assertIn(CANCELLED, [a for _, a in trace])

    def test_wake_then_speech_events_open_and_close(self):
        trace = self.run_core([tick('W'), tick('s'), tick(), tick('e'), tick()], energy=0)
        self.assertEqual([a for _, a in trace if a], [POST_DOWN, POST_UP | SUBMITTED])

    def test_energy_endpointing_without_runner_events(self):
        ticks = [tick('W'), tick(voiced=1)] + [tick()] * (self.END + 1)
        trace = self.run_core(ticks, energy=1)
        self.assertEqual([a for _, a in trace if a], [POST_DOWN, POST_UP | SUBMITTED])

    def test_hard_cap_ends_capture_without_endpointing(self):
        ticks = [tick('W'), tick('s')] + [tick('s')] * (self.CAP + 2)
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[1 + self.CAP][1], POST_UP | SUBMITTED)

    def test_playback_suppresses_wake(self):
        trace = self.run_core([tick('W', playback=1), tick('s'), tick()])
        self.assertEqual(trace[0], (IDLE, 0))

    def test_real_press_cancels_arming_and_converts_capture(self):
        armed = self.run_core([tick('W'), tick(down=1), tick('s')])
        self.assertEqual(armed[1][1], CANCELLED)
        ticks = [tick('W'), tick('s'), tick(down=1), tick(), tick(up=1)]
        trace = self.run_core(ticks, energy=0)
        # The physical release ends it: SUBMITTED alone, no synthetic POST_UP.
        self.assertEqual(trace[-1][1], SUBMITTED)
        self.assertFalse(any(a & POST_UP for _, a in trace))

    def test_disable_closes_an_open_capture_and_cancels_arming(self):
        ticks = [tick('W'), tick('s'), tick(), tick(enabled=0), tick('W', enabled=0)]
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[3][1], POST_UP | SUBMITTED)
        self.assertEqual(trace[4], (IDLE, 0))
        armed = self.run_core([tick('W'), tick(enabled=0)])
        self.assertEqual(armed[1][1], CANCELLED)

    def run_ring(self, cap, chunk, pushes, pops):
        ran = subprocess.run([str(self.binary), 'ring', str(cap), str(chunk),
                              str(pushes), str(pops)], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        popped_line, snap_line, drops_line = ran.stdout.decode().splitlines()
        # deque model of §5.2: overflow drops the oldest frames, counted.
        held, drops, nxt = deque(), 0, 0
        for _ in range(pushes):
            for _ in range(chunk):
                if len(held) == cap:
                    held.popleft()
                    drops += 1
                held.append(nxt)
                nxt += 1
        popped = []
        for _ in range(pops):
            for _ in range(chunk):
                if held:
                    popped.append(held.popleft())
        got_popped = [int(x) for x in popped_line.split()[1:] if x != '-']
        got_snap = [int(x) for x in snap_line.split()[1:]]
        self.assertEqual(got_popped, popped)
        self.assertEqual(got_snap, list(held))
        self.assertEqual(drops_line.split(), ['drops', str(drops), 'fill', str(len(held))])

    def test_ring_drops_oldest_and_snapshots_in_order(self):
        self.run_ring(cap=8, chunk=4, pushes=1, pops=0)      # partial fill
        self.run_ring(cap=8, chunk=4, pushes=3, pops=1)      # overflow by a chunk
        self.run_ring(cap=10, chunk=4, pushes=4, pops=2)     # unaligned wrap
        self.run_ring(cap=320 * 25, chunk=320, pushes=30, pops=5)   # §5.2 size
        self.run_ring(cap=4, chunk=4, pushes=1, pops=3)      # pop past empty

    def run_energy(self, samples, threshold):
        ran = subprocess.run([str(self.binary), 'energy', str(threshold)],
                             input=(' '.join(map(str, samples))).encode(),
                             capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        want = bool(samples) and sum(s * s for s in samples) >= threshold * threshold * len(samples)
        self.assertEqual(int(ran.stdout.decode().strip()), int(want))

    def test_energy_voiced_is_a_mean_square_check(self):
        self.run_energy([], 512)
        self.run_energy([0] * 320, 512)
        self.run_energy([511] * 320, 512)
        self.run_energy([512] * 320, 512)     # exactly at the threshold: voiced
        self.run_energy([-4000] * 320, 512)
        self.run_energy([100] * 319 + [9000], 512)   # one loud sample is not speech energy


class WakeWordBenchRunner(unittest.TestCase):
    """The bench stand-in for the ET runner: a serial trigger and a
    clap trigger. A burst is a short transient (or sustained loud
    sound) spanning at most 8 chunks, then 2 quiet chunks to fire and
    50 feeds of cooldown; constants pinned from ww_runner_bench.h."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'muse_wakeword_bench_harness'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(WW / 'include'),
             str(ROOT / 'tests/muse_wakeword_bench_harness.c'),
             str(WW / 'muse_wakeword_core.c'), str(WW / 'ww_runner_bench.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_bench(self, pattern):
        ran = subprocess.run([str(self.binary), pattern], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        return [int(x) for x in ran.stdout.decode().split()]

    def test_quiet_room_never_wakes(self):
        self.assertEqual(self.run_bench('Q' * 20), [NONE] * 20)

    def test_serial_trigger_fires_once_on_the_next_feed(self):
        self.assertEqual(self.run_bench('TQ'), [WAKE, NONE])
        self.assertEqual(self.run_bench('WQ'), [WAKE, SPEECH_START])

    def test_energy_burst_needs_loud_then_quiet(self):
        self.assertEqual(self.run_bench('LLLQQ'), [NONE] * 4 + [WAKE])
        self.assertEqual(self.run_bench('LQQ'), [NONE, NONE, WAKE])
        self.assertEqual(self.run_bench('LLLLLQ'), [NONE] * 6)     # still loud

    def test_short_clap_transient_wakes_after_quiet(self):
        # Whole-chunk RMS is only 3000: sustained-chunk thresholds missed
        # real claps on the BOX-3, whose energy is in the first few ms.
        self.assertEqual(self.run_bench('CQQ'), [NONE, NONE, WAKE])
        self.assertEqual(self.run_bench('CCCQQ')[-1], WAKE)        # decay across chunks
        self.assertEqual(self.run_bench('cQQ'), [NONE] * 3)        # peak too soft

    def test_sustained_loud_sound_is_not_a_clap(self):
        self.assertEqual(self.run_bench('L' * 8 + 'QQ')[-1], WAKE)
        self.assertEqual(self.run_bench('L' * 9 + 'QQ')[-1], NONE)

    def test_cooldown_blocks_an_immediate_second_burst(self):
        self.assertEqual(self.run_bench('LLLQQ' + 'LLLQQ')[-1], NONE)
        again = self.run_bench('LLLQQ' + 'Q' * 50 + 'LLLQQ')
        self.assertEqual(again[-1], WAKE)

    def test_gates_discard_bursts_and_triggers(self):
        self.assertEqual(self.run_bench('0LLLQQ1Q'), [NONE] * 6)   # not listening
        self.assertEqual(self.run_bench('sLLLQQuQ'), [NONE] * 6)   # suppressed
        self.assertEqual(self.run_bench('sTuQ'), [NONE, NONE])     # trigger dropped

    def test_reset_forgets_a_half_finished_burst(self):
        self.assertEqual(self.run_bench('LLLRQQ'), [NONE] * 5)
        self.assertEqual(self.run_bench('CRQQ'), [NONE] * 3)




def between(source, start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


class WakeWordFirmwareContract(unittest.TestCase):
    """The firmware wiring that only exists as source relationships.
    Behaviour is the other classes' job."""

    def test_synthetic_turns_are_tagged_and_seeded(self):
        self.assertIn('bool synthetic;', (ROOT / 'components/muse/muse_input.h').read_text())
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        self.assertIn('void muse_voice_seed_preroll(', voice)


if __name__ == '__main__':
    unittest.main()
