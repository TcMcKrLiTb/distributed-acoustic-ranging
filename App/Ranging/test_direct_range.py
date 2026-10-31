"""Validate supported early-window ranging and replay available live recordings.

Saved captures can test DSP, not recover a missing board's unrecorded waveform.
Live recordings are optional local artifacts; synthetic safety checks always run.
"""
from __future__ import annotations
import argparse
import ctypes as ct
import json
from pathlib import Path
import random
import re
import statistics
import sys
import tempfile

sys.dont_write_bytecode = True
import test_range_diagnostics as core


def direct(dll, a, b, cfg=None):
    result, support = core.Estimate(), core.Estimate()
    cfg = cfg or core.Config(343420, 0)
    ok = dll.Range_EstimateDirect(ct.byref(a), ct.byref(b), ct.byref(cfg),
                                  ct.byref(result), ct.byref(support))
    if not ok:
        assert result.raw_delay_ns == result.delay_ns == result.distance_mm == 0
    return bool(ok), result, support


def synthetic(dll):
    for lag in (-40, -7, 0, 7, 40):
        for start in (-2_000_000, 2_000_000):
            a, b = core.synthetic(lag, start)
            ok, r, support = direct(dll, a, b)
            assert ok and r.error == support.error == 0
            expected = start + lag * core.PERIOD / 1000
            assert abs(r.raw_delay_ns - expected) < 500
            assert r.source_a == (expected > 0)
            assert r.window_begin == 128 and r.window_end == 480
    a, b = core.synthetic(0, 4_000_000)
    ok, r, _ = direct(dll, a, b, core.Config(343420, 5_000_000))
    assert ok and not r.source_a and r.distance_mm == 343
    b.flags |= 2
    assert not direct(dll, a, b)[0]
    a, b = core.synthetic(0, 8_000_000)
    ok, r, _ = direct(dll, a, b)
    assert not ok and r.error == 5 and r.candidate_distance_mm > 2010
    cfg, r = core.Config(343420, 0), core.Estimate()
    assert not dll.Range_EstimateDirect(None, None, ct.byref(cfg), ct.byref(r), None)
    assert r.error == 1
    a, b = core.synthetic()
    assert not dll.Range_EstimateDirect(ct.byref(a), ct.byref(b), ct.byref(cfg), ct.byref(r), ct.byref(r))
    assert r.error == 1 and r.distance_mm == 0
    print("PASS: signs, bias, physical delay, clipping and formal-field clearing")

    # Early samples match exactly, while the later independent component lowers
    # LONG correlation below its original acceptance gate. The same phase remains.
    a, b = core.synthetic(0, 4_000_000)
    rng = random.Random(19)
    for i in range(512, core.N):
        b.pcm[i] = round(.4 * a.pcm[i] + .916 * rng.randint(-6000, 6000))
    ok, r, support = direct(dll, a, b)
    assert support.error == 3 and 500 <= support.quality_permille < 650
    assert ok and abs(r.raw_delay_ns - 4_000_000) < 500
    print("PASS: later incoherence can reduce LONG without vetoing corroborated direct sound")

    # A common transient alone is insufficient when the rest of the capture has
    # no support, or instead supports a different periodic/echo branch.
    for shifted_tail in (False, True):
        a, b = core.synthetic(0, 4_000_000)
        original = list(a.pcm)
        for i in range(512, core.N):
            b.pcm[i] = original[i - 32] if shifted_tail else rng.randint(-6000, 6000)
        ok, r, support = direct(dll, a, b)
        assert not ok and r.error in (3, 4)
    import math
    a, b = core.synthetic(0)
    a.pcm[:] = b.pcm[:] = [round(6000 * math.sin(2 * math.pi * i / 24)) for i in range(core.N)]
    assert not direct(dll, a, b)[0]
    print("PASS: unsupported transient, conflicting late phase and periodic ambiguity are rejected")

    # One or both coarse triggers precede a shared later burst. Use only known
    # saved samples; starts account for the pulse positions in each capture.
    for onset_a, onset_b in ((700, 256), (256, 700), (700, 920)):
        target = 4_350_000
        start_difference = round(target - (onset_b - onset_a) * core.PERIOD / 1000)
        a, b = core.synthetic(0, start_difference)
        a.pcm[:] = b.pcm[:] = [0] * core.N
        waveform = [rng.randint(-6000, 6000) for _ in range(1100)]
        for capture, onset in ((a, onset_a), (b, onset_b)):
            if onset > 256:
                for i in range(256, 352):
                    capture.pcm[i] = rng.randint(-300, 300)
            for i, value in enumerate(waveform):
                capture.pcm[onset + i] = value
        ok, r, support = direct(dll, a, b)
        assert ok and abs(r.raw_delay_ns - target) < 500, (onset_a, onset_b, ok, r.error, r.raw_delay_ns,
                                                        r.quality_permille, support.error, support.quality_permille)
        assert r.window_begin == support.window_begin
        assert support.window_end - support.window_begin >= 768
    print("PASS: one-sided and two-sided premature triggers can re-align a later shared burst")
    # A coherent but physically out-of-range early burst must not be bypassed
    # just because a later part of the capture could produce a valid distance.
    for i in range(256, 352):
        a.pcm[i] = b.pcm[i] = round(300 * math.sin(2 * math.pi * i / 16))
    ok, r, _ = direct(dll, a, b)
    assert not ok and r.error == 5
    print("PASS: later rescue never bypasses an out-of-range early peak")


def replay(dll, output):
    parent = core.ROOT / "build/direction-test"
    cases = [("old_A", "live_20261030_111853", "01_A_original", 343420),
             ("old_B", "live_20261030_111853", "02_B_original", 343420),
             ("A_T235", "live_20261030_221026", "01_A_original", 345464),
             ("A_unaligned", "live_20261031_203707", "01_A_repeat", 343420),
             ("A_aligned", "live_20261031_205009", "01_A_aligned_T23", 345173),
             ("DIRECT_live", "live_20261031_210947", "02_A_playback_T23", 345173)]
    reports = {}
    for label, session, case, speed in cases:
        folder = parent / session / case
        if not folder.exists():
            print("SKIP: unavailable local capture", folder)
            continue
        boards = {"A": [], "B": []}
        for p in folder.glob("COM*_*.json"):
            meta = json.loads(p.read_text(encoding="utf-8-sig"))
            boards[meta["board"]].append((p.name, core.load_capture(p)))
        used, records = set(), []
        for aname, a in sorted(boards["A"], key=lambda item: item[1].onset_ns):
            candidates = [(n, b) for n, b in boards["B"] if n not in used and
                          abs(int(a.onset_ns) - int(b.onset_ns)) <= 20_000_000]
            if len(candidates) != 1:
                continue
            bname, b = candidates[0]
            used.add(bname)
            ok, r, support = direct(dll, a, b, core.Config(speed, 0))
            records.append(dict(a=a.sequence, b=b.sequence, a_file=aname, b_file=bname,
                                ok=ok, mm=r.distance_mm, error=r.error, q=r.quality_permille,
                                delay_ns=r.raw_delay_ns, begin=r.window_begin, end=r.window_end,
                                long_q=support.quality_permille))
        if label == "A_aligned":
            sound = [r for r in records if 128 <= r["a"] <= 150]
            assert len(sound) == 22 and all(r["ok"] for r in sound)
            assert all(1490 <= r["mm"] <= 1500 for r in sound)
            assert all(not r["ok"] for r in records if r["a"] < 128)
        if label == "A_unaligned":
            assert not next(r for r in records if r["a"] == 65)["ok"], "879 mm non-periodic event must be rejected"
        if label == "old_B":
            assert all(1200 <= r["mm"] <= 1350 for r in records if r["ok"]), "known periodic aliases must not become accepted"
        if label == "DIRECT_live":
            sound = [r for r in records if 19 <= r["a"] <= 42]
            assert len(sound) == 24 and all(r["ok"] for r in sound)
            assert all(1490 <= r["mm"] <= 1510 for r in sound)
            assert all(not r["ok"] for r in records if r["a"] < 19 or r["a"] > 42)
            # Preserve every already accepted hardware result bit-for-bit in
            # the reported integer metrics; only the rejected late burst changes.
            firmware = {}
            for line in (folder / "COM6.log").read_text(encoding="utf-8-sig").splitlines():
                if not line.startswith("RANGE CMP "): continue
                fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
                if fields["win"] == "DIRECT" and fields["error"] == "0":
                    firmware[int(fields["a"])] = fields
            assert len(firmware) == 23
            for row in records:
                if row["a"] in firmware:
                    f = firmware[row["a"]]
                    assert (row["error"], row["q"], row["delay_ns"], row["mm"]) == (0, int(f["q"]), int(f["dt_ns"]), int(f["mm"]))
        values = [r["mm"] for r in records if r["ok"]]
        print(label, f"accepted={len(values)}/{len(records)} saved pairs",
              f"range={min(values)}..{max(values)} mm mean={statistics.mean(values):.3f}" if values else "")
        reports[label] = records
    (output / "direct_replay.json").write_text(json.dumps(reports, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    parent = core.ROOT / "build/range-validation"
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="direct-", dir=parent))
    dll = core.build_variant(args.cc, run, "current", (core.ROOT / "App/Ranging/range_core.c").read_bytes(),
                             (core.ROOT / "App/Ranging/range_core.h").read_bytes())
    dll.Range_EstimateDirect.argtypes = [ct.POINTER(core.Capture), ct.POINTER(core.Capture),
                                        ct.POINTER(core.Config), ct.POINTER(core.Estimate), ct.POINTER(core.Estimate)]
    dll.Range_EstimateDirect.restype = ct.c_bool
    synthetic(dll)
    replay(dll, run)
    print("All direct-window checks passed. Artifacts:", run)


if __name__ == "__main__":
    main()
