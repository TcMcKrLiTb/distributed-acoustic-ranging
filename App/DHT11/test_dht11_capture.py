"""Exercise the real DHT11 edge decoder without MCU hardware."""

from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "App" / "DHT11"
BUILD = ROOT / "build" / "dht11-validation" / "capture"

HARNESS = r'''
#include "dht11_capture.h"
#include <stdio.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(condition) do { assertions++; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void frame(uint16_t start, const uint8_t bytes[5], bool jitter) {
    DHT11Capture capture;
    DHT11Capture_Begin(&capture, (uint16_t)(start - 500U));
    DHT11Capture_Edge(&capture, start, true);
    CHECK(capture.started_us == start); /* Task delay before release is harmless. */
    unsigned now = start;
    DHT11Capture_Edge(&capture, (uint16_t)(now += 20U), false);
    DHT11Capture_Edge(&capture, (uint16_t)(now += 80U), true);
    DHT11Capture_Edge(&capture, (uint16_t)(now += 80U), false);
    for (unsigned bit = 0; bit < 40U; bit++) {
        now += 50U;
        DHT11Capture_Edge(&capture, (uint16_t)(now + (jitter ? 4U : 0U)), true);
        now += bytes[bit / 8U] & (0x80U >> (bit % 8U)) ? 70U : 26U;
        DHT11Capture_Edge(&capture, (uint16_t)now, false);
    }
    CHECK(capture.phase == DHT11_CAPTURE_DONE);
    CHECK(capture.error == DHT11_CAPTURE_OK && capture.bits == 40U);
    CHECK(memcmp(capture.data, bytes, 5U) == 0);
    CHECK(capture.response_us == (uint16_t)(now - start));
    DHT11Capture_Edge(&capture, (uint16_t)(now + 50U), true);
    DHT11Capture_Timeout(&capture, (uint16_t)(now + 10000U));
    CHECK(capture.phase == DHT11_CAPTURE_DONE); /* Trailing release is ignored. */
}

static DHT11Capture header(void) {
    DHT11Capture capture;
    DHT11Capture_Begin(&capture, 0U);
    DHT11Capture_Edge(&capture, 0U, true);
    DHT11Capture_Edge(&capture, 20U, false);
    DHT11Capture_Edge(&capture, 100U, true);
    DHT11Capture_Edge(&capture, 180U, false);
    return capture;
}

static void test_bad_waveforms(void) {
    DHT11Capture capture;
    DHT11Capture_Begin(&capture, 0U);
    DHT11Capture_Edge(&capture, 20U, false);
    CHECK(capture.error == DHT11_CAPTURE_SEQUENCE);
    DHT11Capture_Edge(&capture, 30U, true);
    CHECK(capture.phase == DHT11_CAPTURE_FAILED);

    DHT11Capture_Begin(&capture, 0U);
    DHT11Capture_Edge(&capture, 0U, true);
    DHT11Capture_Edge(&capture, 121U, false);
    CHECK(capture.error == DHT11_CAPTURE_PULSE);

    const unsigned bad_response[] = {49U, 111U};
    for (unsigned i = 0; i < 2U; i++) {
        DHT11Capture_Begin(&capture, 0U);
        DHT11Capture_Edge(&capture, 0U, true);
        DHT11Capture_Edge(&capture, 20U, false);
        DHT11Capture_Edge(&capture, (uint16_t)(20U + bad_response[i]), true);
        CHECK(capture.error == DHT11_CAPTURE_PULSE);
    }
    const unsigned bad_low[] = {29U, 86U};
    for (unsigned i = 0; i < 2U; i++) {
        capture = header();
        DHT11Capture_Edge(&capture, (uint16_t)(180U + bad_low[i]), true);
        CHECK(capture.error == DHT11_CAPTURE_PULSE);
    }
    const unsigned bad_high[] = {9U, 46U, 50U, 54U, 101U};
    for (unsigned i = 0; i < sizeof(bad_high) / sizeof(bad_high[0]); i++) {
        capture = header();
        DHT11Capture_Edge(&capture, 230U, true);
        DHT11Capture_Edge(&capture, (uint16_t)(230U + bad_high[i]), false);
        CHECK(capture.error == DHT11_CAPTURE_PULSE);
    }
    capture = header();
    DHT11Capture_Edge(&capture, 230U, false);
    CHECK(capture.error == DHT11_CAPTURE_SEQUENCE);
    capture = header();
    DHT11Capture_Edge(&capture, 7000U, true);
    CHECK(capture.error == DHT11_CAPTURE_TIMEOUT);
    for (unsigned phase = 0; phase < 3U; phase++) {
        DHT11Capture_Begin(&capture, 65530U);
        if (phase >= 1U) DHT11Capture_Edge(&capture, 65530U, true);
        if (phase >= 2U) DHT11Capture_Edge(&capture, 14U, false);
        DHT11Capture_Timeout(&capture, 9994U);
        CHECK(capture.phase == DHT11_CAPTURE_FAILED);
        CHECK(capture.error == DHT11_CAPTURE_TIMEOUT);
    }
}

int main(void) {
    uint8_t bytes[5];
    for (unsigned byte = 0; byte < 5U; byte++) {
        for (unsigned value = 0; value < 256U; value++) {
            memset(bytes, 0xA5, sizeof(bytes));
            bytes[byte] = (uint8_t)value;
            uint16_t start = (uint16_t)(65530U + value * 257U + byte * 71U);
            frame(start, bytes, false);
            frame(start, bytes, true);
        }
    }
    test_bad_waveforms();
    printf("DHT11 edge decoder: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
'''


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "capture_harness.c"
    harness.write_text(HARNESS, encoding="utf-8")
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler:
        raise SystemExit("Native GCC is required (or set CC).")
    executable = BUILD / ("test_dht11_capture.exe" if os.name == "nt" else "test_dht11_capture")
    subprocess.run([
        compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(PORT), str(PORT / "dht11_capture.c"), str(harness),
        "-o", str(executable),
    ], check=True, cwd=ROOT)
    subprocess.run([str(executable)], check=True, cwd=ROOT, timeout=10)


if __name__ == "__main__":
    main()
