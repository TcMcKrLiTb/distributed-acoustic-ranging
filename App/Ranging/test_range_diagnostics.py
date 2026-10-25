"""Host checks for the real ranging core; no board or third-party Python package.

Builds current C plus the fixed diagnostic snapshot 3363294 as the 96-sample
baseline. A controlled copy of current C using the snapshot's search width
checks compatibility separately from the intentional wider-search behavior.
Artifacts are retained under build/range-validation for inspection.
"""
from __future__ import annotations

import argparse
import copy
import ctypes as ct
import json
from pathlib import Path
import random
import re
import statistics
import struct
import subprocess
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[2]
N = 2048
PERIOD = 20_833_333


class Capture(ct.Structure):
    _fields_ = [("sequence", ct.c_uint32), ("generation", ct.c_uint32),
                ("flags", ct.c_uint32), ("onset_ns", ct.c_uint64),
                ("start_ns", ct.c_uint64), ("period_ps", ct.c_uint64),
                ("pcm", ct.c_int16 * N)]


class Config(ct.Structure):
    _fields_ = [("sound_speed_mm_s", ct.c_uint32), ("bias_ns", ct.c_int32)]


class Estimate(ct.Structure):
    _fields_ = [("raw_delay_ns", ct.c_int64), ("delay_ns", ct.c_int64),
                ("distance_mm", ct.c_uint32), ("quality_permille", ct.c_uint32),
                ("peak_lag_samples", ct.c_int32), ("correlation_valid", ct.c_bool),
                ("source_a", ct.c_bool), ("error", ct.c_int),
                ("window_begin", ct.c_uint16), ("window_end", ct.c_uint16),
                ("competitor_quality_permille", ct.c_uint32),
                ("competitor_lag_samples", ct.c_int32), ("peak_margin_permille", ct.c_int32),
                ("competitor_valid", ct.c_bool), ("peak_fraction_samples", ct.c_float),
                ("candidate_valid", ct.c_bool), ("candidate_raw_delay_ns", ct.c_int64),
                ("candidate_distance_mm", ct.c_uint32)]


FORMAL = ("raw_delay_ns", "delay_ns", "distance_mm", "quality_permille",
          "peak_lag_samples", "correlation_valid", "source_a", "error")


def build_variant(cc: str, run: Path, name: str, source: bytes, header: bytes):
    folder = run / name
    folder.mkdir()
    (folder / "range_core.c").write_bytes(source)
    (folder / "range_core.h").write_bytes(header)
    probe = folder / "layout.c"
    probe.write_text('#include "range_core.h"\n#include <stddef.h>\n'
                     'size_t size_capture(void) { return sizeof(RangeCapture); }\n'
                     'size_t size_estimate(void) { return sizeof(RangeEstimate); }\n'
                     'size_t offset_candidate(void) { return offsetof(RangeEstimate, candidate_raw_delay_ns); }\n'
                     'unsigned search_samples(void) { return RANGE_SEARCH_SAMPLES; }\n')
    output = folder / (name + ".dll")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared",
                    str(folder / "range_core.c"), str(probe), "-o", str(output), "-lm"], check=True)
    dll = ct.CDLL(str(output))
    for func in ("size_capture", "size_estimate", "offset_candidate"):
        getattr(dll, func).restype = ct.c_size_t
    dll.search_samples.restype = ct.c_uint
    assert dll.size_capture() == ct.sizeof(Capture)
    assert dll.size_estimate() == ct.sizeof(Estimate)
    assert dll.offset_candidate() == Estimate.candidate_raw_delay_ns.offset
    dll.Range_Estimate.argtypes = [ct.POINTER(Capture), ct.POINTER(Capture),
                                   ct.POINTER(Config), ct.POINTER(Estimate)]
    dll.Range_Estimate.restype = ct.c_bool
    dll.Range_EstimateWindow.argtypes = [ct.POINTER(Capture), ct.POINTER(Capture),
                                         ct.POINTER(Config), ct.c_uint, ct.c_uint,
                                         ct.POINTER(Estimate)]
    dll.Range_EstimateWindow.restype = ct.c_bool
    return dll


def build(cc: str, run: Path, baseline_ref: str):
    include = ROOT / "App/Ranging"
    current_source = (include / "range_core.c").read_bytes()
    current_header = (include / "range_core.h").read_bytes()
    source = subprocess.check_output(["git", "show", baseline_ref + ":App/Ranging/range_core.c"], cwd=ROOT)
    header = subprocess.check_output(["git", "show", baseline_ref + ":App/Ranging/range_core.h"], cwd=ROOT)
    current = build_variant(cc, run, "current", current_source, current_header)
    baseline = build_variant(cc, run, "snapshot", source, header)
    control_header = re.sub(rb"(#define RANGE_SEARCH_SAMPLES)\s+\d+", rb"\g<1> 96", current_header)
    controlled = build_variant(cc, run, "current_search96", current_source, control_header)
    assert current.search_samples() == 127, "current production search must be +/-127 for this regression"
    assert baseline.search_samples() == controlled.search_samples() == 96
    return current, baseline, controlled


def call(dll, a, b, cfg=None, window=None, poison=False):
    cfg = cfg or Config(343420, 0)
    result = Estimate()
    if poison:
        ct.memset(ct.byref(result), 0x55, ct.sizeof(result))
    aa = ct.byref(a) if a is not None else None
    bb = ct.byref(b) if b is not None else None
    if window is None:
        ok = dll.Range_Estimate(aa, bb, ct.byref(cfg), ct.byref(result))
    else:
        ok = dll.Range_EstimateWindow(aa, bb, ct.byref(cfg), *window, ct.byref(result))
    return bool(ok), result


def formal(r):
    return tuple(getattr(r, name) for name in FORMAL)


def synthetic(lag=7, start_difference=2_000_000):
    rng = random.Random(781)
    data = [rng.randint(-6000, 6000) for _ in range(4096)]
    start = 10_000_000_000
    a = Capture(1, 0, 1, start + 256 * PERIOD // 1000, start, PERIOD)
    b = Capture(2, 3, 1, start + start_difference + 256 * PERIOD // 1000,
                start + start_difference, PERIOD)
    a.pcm[:] = data[256:256+N]
    b.pcm[:] = data[256-lag:256-lag+N]
    return a, b


def check_synthetic(current, legacy):
    cfg = Config(343420, 0)
    for lag in (-40, -7, 0, 7, 40):
        a, b = synthetic(lag)
        ok, r = call(current, a, b)
        old_ok, old = call(legacy, a, b)
        assert ok and old_ok and formal(r) == formal(old), (lag, formal(r), formal(old))
        assert r.peak_lag_samples == lag and r.error == 0
        expected_dt = 2_000_000 + lag * PERIOD / 1000
        assert abs(r.raw_delay_ns - expected_dt) < 300
        assert r.source_a and r.candidate_valid
        assert r.candidate_raw_delay_ns == r.raw_delay_ns
        assert r.candidate_distance_mm == r.distance_mm
        assert r.competitor_valid and r.peak_margin_permille > 20
        ok2, rbias = call(current, a, b, Config(343420, 4_000_000))
        assert ok2 and not rbias.source_a
        assert rbias.delay_ns == r.raw_delay_ns - 4_000_000
        assert rbias.candidate_raw_delay_ns == r.raw_delay_ns
        assert abs(rbias.distance_mm - abs(rbias.delay_ns) * 343420 / 1e9) <= .5
    a, b = synthetic(7, -2_000_000)
    ok, r = call(current, a, b)
    assert ok and not r.source_a and r.raw_delay_ns < 0

    # A's right endpoint is excluded. B still supplies interpolation support.
    a, b = synthetic()
    ok, r = call(current, a, b, window=(128, 384))
    a.pcm[384] = 32000
    ok2, r2 = call(current, a, b, window=(128, 384))
    assert ok == ok2 and bytes(r) == bytes(r2)
    a.pcm[383] = 32000
    _, r3 = call(current, a, b, window=(128, 384))
    assert r3.quality_permille != r.quality_permille

    # Former boundary peaks become valid interior peaks in the wider search.
    for lag in (-96, 96):
        a, b = synthetic(lag, 3_000_000)
        ok, r = call(current, a, b)
        old_ok, old = call(legacy, a, b)
        assert ok and r.error == 0 and r.peak_lag_samples == lag
        assert not old_ok and old.error == 4 and not old.candidate_valid
    for lag in (-127, 127):
        a, b = synthetic(lag)
        ok, r = call(current, a, b)
        assert not ok and r.error == 4 and r.peak_lag_samples == lag
        assert r.correlation_valid and not r.candidate_valid
        assert r.raw_delay_ns == r.distance_mm == 0

    import math
    a, b = synthetic(0)
    tone = [round(6000 * math.sin(2 * math.pi * i / 24)) for i in range(N)]
    a.pcm[:] = tone
    b.pcm[:] = tone
    ok, r = call(current, a, b)
    old_ok, old = call(legacy, a, b)
    assert not old_ok and old.error == 4
    assert not ok and r.error == 4 and r.peak_margin_permille < 20
    assert r.raw_delay_ns == r.distance_mm == 0

    # A out-of-range but coherent pair keeps a candidate and is still rejected.
    a, b = synthetic(0, 8_000_000)
    ok, r = call(current, a, b)
    old_ok, old = call(legacy, a, b)
    assert (ok, formal(r)) == (old_ok, formal(old))
    assert not ok and r.error == 5 and r.candidate_valid and r.candidate_distance_mm > 2010
    print("PASS: synthetic sign/bias, legacy parity, exclusive endpoint, boundary/periodic ambiguity, physical range")


def check_invalid(current):
    a, b = synthetic()
    for window in [(0, 384), (128, 128), (128, 130), (128, 2049), (128, 2048), (2048, 384)]:
        ok, r = call(current, a, b, window=window, poison=True)
        assert not ok and r.error == 1 and not r.correlation_valid and not r.candidate_valid
        assert r.raw_delay_ns == r.distance_mm == r.candidate_raw_delay_ns == 0
    for field, value in [("flags", 3), ("period_ps", 0), ("onset_ns", 2**63),
                         ("start_ns", a.onset_ns+1), ("onset_ns", a.start_ns+10_000_001)]:
        bad = copy.copy(a)
        setattr(bad, field, value)
        ok, r = call(current, bad, b, poison=True)
        assert not ok and r.error == 1 and not r.correlation_valid and not r.candidate_valid
        assert r.quality_permille == r.candidate_raw_delay_ns == 0
    aa, bb = copy.copy(a), copy.copy(b)
    aa.period_ps, bb.period_ps = 22_000_000, 20_000_000
    ok, r = call(current, aa, bb, poison=True)
    assert not ok and r.error == 1 and not r.correlation_valid
    for aa, bb in [(None, b), (a, None)]:
        ok, r = call(current, aa, bb, poison=True)
        assert not ok and r.error == 1 and not r.candidate_valid
    for cfg in [Config(299999, 0), Config(380001, 0), Config(343420, 10_000_001)]:
        ok, r = call(current, a, b, cfg, poison=True)
        assert not ok and r.error == 1 and not r.candidate_valid
    r = Estimate()
    ct.memset(ct.byref(r), 0x55, ct.sizeof(r))
    assert not current.Range_EstimateWindow(ct.byref(a), ct.byref(b), None, 128, 384, ct.byref(r))
    assert r.error == 1 and not r.correlation_valid and not r.candidate_valid
    assert not current.Range_Estimate(ct.byref(a), ct.byref(b), ct.byref(Config(343420, 0)), None)
    a.pcm[:] = [0]*N
    ok, r = call(current, a, b, poison=True)
    assert not ok and r.error == 2 and not r.correlation_valid and not r.candidate_valid
    a, b = synthetic()
    b.pcm[:] = [0]*N
    ok, r = call(current, a, b, poison=True)
    assert not ok and r.error == 3 and r.correlation_valid and not r.candidate_valid
    assert r.raw_delay_ns == r.distance_mm == 0
    print("PASS: invalid windows/metadata/config/nulls, interpolation-domain guard, silence; stale diagnostics cleared")


def load_capture(path):
    m = json.loads(path.read_text(encoding="utf-8-sig"))
    raw = path.with_suffix(".pcm").read_bytes()
    assert len(raw) == N*2 and f"{zlib.crc32(raw):08x}" == m["crc32"]
    return Capture(int(m["sequence"]), int(m["epoch"]), int(m["flags"]),
                   int(m["onset_ns"]), int(m["start_ns"]), int(m["period_ps"]),
                   (ct.c_int16*N)(*struct.unpack("<2048h", raw)))


def check_recordings(current, legacy, controlled, folder):
    if not folder.is_dir():
        print("SKIP: real PCM fixture missing:", folder)
        return
    aa = sorted((load_capture(p) for p in folder.glob("COM3_*.json")), key=lambda c:c.onset_ns)
    bb = sorted((load_capture(p) for p in folder.glob("COM8_*.json")), key=lambda c:c.onset_ns)
    assert len(aa) == len(bb) == 24
    text = (folder/"COM3.log").read_text(encoding="utf-8-sig")
    logged = [(int(q), int(k)) for q,k in re.findall(r"quality=(\d+) lag=(-?\d+)",text)]
    assert len(logged) == 24
    front = []
    used_b = set()
    for index, a in enumerate(aa):
        b = min(bb, key=lambda c:abs(c.onset_ns-a.onset_ns))
        assert abs(a.onset_ns-b.onset_ns) < 20_000_000 and b.sequence not in used_b
        used_b.add(b.sequence)
        # Equal-width replay is the compatibility check. Wide-search results
        # are intentionally allowed to change and are archived by the corpus test.
        ok, r = call(controlled, a, b)
        old_ok, old = call(legacy, a, b)
        assert (ok, formal(r)) == (old_ok, formal(old))
        assert (r.quality_permille, r.peak_lag_samples) == logged[index]
        assert not ok and r.error == 3 and r.candidate_valid
        assert r.raw_delay_ns == r.delay_ns == r.distance_mm == 0
        ok, rf = call(current, a, b, window=(128, 384))
        assert ok and rf.error == 0 and rf.source_a is False
        assert 698 <= rf.distance_mm <= 703 and 949 <= rf.quality_permille <= 965
        assert rf.candidate_valid and rf.candidate_distance_mm == rf.distance_mm
        front.append(rf.distance_mm)
    print("PASS: controlled +/-96 LONG matches snapshot C and all 24 firmware q/lag values")
    print(f"PASS: all 24 FRONT accepted; {min(front)}..{max(front)} mm, "
          f"mean={statistics.mean(front):.2f}, sample_sd={statistics.stdev(front):.3f} mm")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--baseline-ref", default="3363294",
                        help="fixed diagnostic snapshot with its own source/header (default: 3363294)")
    parser.add_argument("--recordings", type=Path,
                        default=ROOT/"build/pcm-capture/20261023_214834_384")
    args = parser.parse_args()
    parent = ROOT/"build/range-validation"
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    current, legacy, controlled = build(args.cc, run, args.baseline_ref)
    check_synthetic(current, legacy)
    check_invalid(current)
    check_recordings(current, legacy, controlled, args.recordings)
    print("All range diagnostic checks passed. Baseline:", args.baseline_ref, "Artifacts:", run)


if __name__ == "__main__":
    main()
