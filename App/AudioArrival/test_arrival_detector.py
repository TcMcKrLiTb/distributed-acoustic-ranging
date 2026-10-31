"""Exercise real C re-arming with transients, echoes, noise and sustained sound."""
from __future__ import annotations
import argparse
import ctypes as ct
from pathlib import Path
import subprocess
import tempfile


class Detection(ct.Structure):
    _fields_ = [("sample", ct.c_uint64), ("fraction_q16", ct.c_uint16),
                ("peak", ct.c_uint32), ("threshold", ct.c_uint32),
                ("noise", ct.c_uint32), ("confirmation_samples", ct.c_uint32)]


class Detector(ct.Structure):
    _fields_ = [("previous_input", ct.c_int32), ("highpass", ct.c_int32),
                *[(n, ct.c_uint32) for n in ("previous_abs", "noise_q8", "threshold", "warmup", "refractory",
                    "quiet", "candidate_age", "candidate_quiet", "hits", "crossings", "peak")],
                ("previous_sign", ct.c_int32), ("candidate", ct.c_bool), ("blocked", ct.c_bool),
                ("onset", Detection)]


TEST = r'''
#include "arrival_detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static ArrivalDetector d;
static uint64_t sample;
static unsigned events;
static void feed(unsigned n, unsigned amplitude) {
    for (unsigned i = 0; i < n; ++i) {
        int16_t x = (int16_t)lround(amplitude * sin(6.283185307179586 * i / 16));
        ArrivalDetection e;
        if (ArrivalDetector_Push(&d, x, sample++, &e)) {
            CHECK(e.peak >= 2 * e.threshold && e.confirmation_samples >= 96);
            events++;
        }
    }
}
static void reset(void) {
    ArrivalDetector_Reset(&d); sample = 0; events = 0;
    feed(48000, 0); CHECK(ArrivalDetector_Armed(&d));
}
int main(void) {
    reset();
    feed(192, 400); CHECK(events == 1 && d.blocked);
    feed(416 * 48 - 192, 0);
    CHECK(ArrivalDetector_Armed(&d));
    feed(3840, 1800); CHECK(events == 2);
    puts("PASS: short unrelated event does not mask a sound 416 ms later");

    reset(); feed(192, 1800); feed(60 * 48, 0);
    feed(192, 1800); CHECK(events == 1);
    feed(2 * 48000, 0); feed(3840, 1800); CHECK(events == 2);
    puts("PASS: immediate echo is suppressed; a later test sound is accepted");

    reset(); feed(2 * 48000, 1800); CHECK(events == 1 && d.blocked);
    feed(6000, 0); CHECK(ArrivalDetector_Armed(&d));
    feed(3840, 1800); CHECK(events == 2);
    puts("PASS: sustained sound never repeats; quiet is required to re-arm");

    reset();
    for (unsigned i = 0; i < 96000; ++i) {
        ArrivalDetection e;
        int16_t x = (int16_t)((i * 7919U) % 41U) - 20;
        CHECK(!ArrivalDetector_Push(&d, x, sample++, &e));
    }
    ArrivalDetection e;
    CHECK(!ArrivalDetector_Push(&d, 10000, sample++, &e));
    feed(48000, 0); CHECK(events == 0);
    puts("PASS: low-level noise and a one-sample impulse do not become sound events");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    parent = root / "build/arrival-validation"
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    source = run / "test.c"
    source.write_text(TEST, encoding="ascii")
    exe = run / "arrival_test.exe"
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I" + str(root / "App/AudioArrival"), str(source),
                    str(root / "App/AudioArrival/arrival_detector.c"), "-o", str(exe), "-lm"], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
