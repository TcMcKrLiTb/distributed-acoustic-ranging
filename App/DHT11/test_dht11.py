"""Compile the real LibDriver decoder and feed it timed DHT11 bus waveforms.

Requires a native GCC (CC may override it). Generated files stay under build/.
This checks protocol/error handling; it does not verify MCU execution timing.
"""

from pathlib import Path
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "dht11-validation"
DRIVER = ROOT / "Drivers" / "LibDriver" / "DHT11"

HARNESS = r'''
#include "driver_dht11.h"
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum Mode { NORMAL, ABSENT, SHORT_LOW, DATA_LOW, DATA_HIGH };
enum Api { COMBINED, TEMPERATURE, HUMIDITY };
static uint64_t now_us, release_us, low_us;
static uint8_t payload[5];
static bool driving_low, irq_masked, saved_mask, in_critical;
static unsigned disabled, enabled, assertions, failures, read_calls, fail_read;
static unsigned zero_high = 26U, one_high = 70U;
static enum Mode mode;

#define CHECK(condition) do { assertions++; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static uint8_t bus_init(void) { return 0; }
static uint8_t bus_deinit(void) { driving_low = false; return 0; }
static uint8_t bus_write(uint8_t value) {
    if (value == 0U) {
        driving_low = true;
        low_us = now_us;
    } else {
        CHECK(driving_low);
        CHECK(now_us - low_us >= 18000U);
        driving_low = false;
        release_us = now_us;
    }
    return 0;
}
static uint8_t bus_read(uint8_t *value) {
    if (++read_calls == fail_read) return 1;
    if (driving_low || mode == SHORT_LOW) { *value = 0; return 0; }
    if (mode == ABSENT) { *value = 1; return 0; }
    uint64_t elapsed = now_us - release_us;
    if (elapsed < 20U) { *value = 1; return 0; }
    if (elapsed < 100U) { *value = 0; return 0; }
    if (elapsed < 180U) { *value = 1; return 0; }
    if (mode == DATA_LOW) { *value = 0; return 0; }
    if (mode == DATA_HIGH) { *value = 1; return 0; }
    elapsed -= 180U;
    for (unsigned bit = 0; bit < 40U; ++bit) {
        if (elapsed < 50U) { *value = 0; return 0; }
        elapsed -= 50U;
        bool one = (payload[bit / 8U] & (0x80U >> (bit % 8U))) != 0;
        unsigned high = one ? one_high : zero_high;
        if (elapsed < high) { *value = 1; return 0; }
        elapsed -= high;
    }
    *value = elapsed < 50U ? 0U : 1U; /* End-of-frame low, then release. */
    return 0;
}
static void delay_us(uint32_t us) { now_us += us; }
static void delay_ms(uint32_t ms) {
    CHECK(!in_critical); /* The 20 ms start pulse must remain preemptible. */
    now_us += (uint64_t)ms * 1000U;
}
static void disable_irq(void) {
    CHECK(!in_critical);
    in_critical = true;
    saved_mask = irq_masked;
    irq_masked = true;
    disabled++;
}
static void enable_irq(void) {
    CHECK(in_critical);
    in_critical = false;
    irq_masked = saved_mask;
    enabled++;
}
static void debug_print(const char *const fmt, ...) { (void)fmt; }
static dht11_handle_t make_handle(void) {
    dht11_handle_t handle = {
        .bus_init = bus_init, .bus_deinit = bus_deinit,
        .bus_read = bus_read, .bus_write = bus_write,
        .delay_ms = delay_ms, .delay_us = delay_us,
        .disable_irq = disable_irq, .enable_irq = enable_irq,
        .debug_print = debug_print
    };
    return handle;
}
static void reset_bus(enum Mode next, bool masked) {
    now_us = release_us = low_us = 0;
    mode = next;
    driving_low = in_critical = false;
    irq_masked = masked;
    disabled = enabled = read_calls = fail_read = 0;
    zero_high = 26U; one_high = 70U;
}
static void frame(uint8_t humidity, uint8_t fraction, uint8_t temperature,
                  uint8_t temperature_fraction, bool corrupt) {
    payload[0] = humidity; payload[1] = fraction;
    payload[2] = temperature; payload[3] = temperature_fraction;
    payload[4] = (uint8_t)(humidity + fraction + temperature + temperature_fraction);
    if (corrupt) payload[4] ^= 0x01U;
}
static uint8_t read_api(dht11_handle_t *handle, enum Api api,
                        uint16_t *tr, float *t, uint16_t *hr, uint8_t *h) {
    if (api == TEMPERATURE) return dht11_read_temperature(handle, tr, t);
    if (api == HUMIDITY) return dht11_read_humidity(handle, hr, h);
    return dht11_read_temperature_humidity(handle, tr, t, hr, h);
}
static void check_balanced(bool initial_mask) {
    CHECK(disabled == enabled);
    CHECK(!in_critical);
    CHECK(irq_masked == initial_mask);
}
static void good_frame(uint8_t h, uint8_t hf, uint8_t t, uint8_t tf,
                       float expected, enum Api api, bool initial_mask) {
    reset_bus(NORMAL, initial_mask);
    frame(h, hf, t, tf, false);
    dht11_handle_t handle = make_handle();
    CHECK(dht11_init(&handle) == 0);
    now_us += 2000000U; /* Separate init's handshake from the first sample. */
    uint16_t tr = 0xFFFFU, hr = 0xFFFFU;
    float temperature = -999.0f;
    uint8_t humidity = 255U;
    CHECK(read_api(&handle, api, &tr, &temperature, &hr, &humidity) == 0);
    if (api != HUMIDITY) {
        CHECK(tr == ((uint16_t)t << 8 | tf));
        CHECK(fabsf(temperature - expected) < 0.001f);
    }
    if (api != TEMPERATURE) {
        CHECK(hr == ((uint16_t)h << 8 | hf));
        CHECK(humidity == h); /* Upstream API intentionally returns integer RH. */
    }
    CHECK(dht11_deinit(&handle) == 0);
    CHECK(handle.inited == 0U);
    check_balanced(initial_mask);
}
static void bad_frame(enum Mode failure_mode, bool corrupt, enum Api api,
                      unsigned injected_read) {
    reset_bus(NORMAL, false);
    frame(56U, 0U, 25U, 3U, corrupt);
    dht11_handle_t handle = make_handle();
    CHECK(dht11_init(&handle) == 0);
    now_us += 2000000U;
    mode = failure_mode;
    read_calls = 0; fail_read = injected_read;
    uint16_t tr = 0xABCDU, hr = 0x1234U;
    float temperature = -999.0f;
    uint8_t humidity = 255U;
    uint64_t start = now_us;
    CHECK(read_api(&handle, api, &tr, &temperature, &hr, &humidity) != 0);
    CHECK(tr == 0xABCDU && hr == 0x1234U);
    CHECK(temperature == -999.0f && humidity == 255U);
    CHECK(now_us - start < 27000U); /* Failure must terminate in a bounded time. */
    check_balanced(false);
}
int main(void) {
    for (enum Api api = COMBINED; api <= HUMIDITY; ++api) {
        good_frame(56U, 0U, 25U, 3U, 25.3f, api, false);
        good_frame(85U, 1U, 3U, 0x84U, -3.4f, api, true);
        good_frame(100U, 0U, 20U, 0x89U, -20.9f, api, false); /* Sum=257. */
        bad_frame(NORMAL, true, api, 0);
        bad_frame(ABSENT, false, api, 0);
        bad_frame(SHORT_LOW, false, api, 0);
        bad_frame(DATA_LOW, false, api, 0);
        bad_frame(DATA_HIGH, false, api, 0);
        bad_frame(NORMAL, false, api, 1);   /* First reset read. */
        bad_frame(NORMAL, false, api, 100); /* During the payload. */
    }
    for (enum Mode failure = ABSENT; failure <= SHORT_LOW; ++failure) {
        reset_bus(failure, true);
        dht11_handle_t handle = make_handle();
        CHECK(dht11_init(&handle) == 4U);
        CHECK(handle.inited == 0U);
        CHECK(now_us < 21000U);
        check_balanced(true);
    }
    reset_bus(NORMAL, false);
    dht11_handle_t handle = make_handle();
    uint16_t raw = 0; float temperature = 0;
    CHECK(dht11_init(NULL) == 2U);
    CHECK(dht11_read_temperature(&handle, &raw, &temperature) == 3U);
    CHECK(dht11_deinit(&handle) == 3U);
    handle.delay_us = NULL;
    CHECK(dht11_init(&handle) == 3U);
    printf("DHT11 protocol: %u assertions, %u failures\n", assertions, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
'''


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / "protocol_test.c"
    source.write_text(HARNESS, encoding="utf-8")
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler:
        raise SystemExit("Native GCC not found; set CC to its executable path.")
    executable = BUILD / ("protocol_test.exe" if os.name == "nt" else "protocol_test")
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         "-I", str(DRIVER), str(source), str(DRIVER / "driver_dht11.c"),
         "-lm", "-o", str(executable)], check=True
    )
    subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
