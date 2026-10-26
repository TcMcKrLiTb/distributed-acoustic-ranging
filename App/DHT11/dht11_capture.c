#include "dht11_capture.h"
#include <string.h>

#define RESPONSE_LIMIT_US 6000U

static void fail(DHT11Capture *capture, DHT11CaptureError error) {
    capture->error = error;
    capture->phase = DHT11_CAPTURE_FAILED;
}

void DHT11Capture_Begin(DHT11Capture *capture, uint16_t now) {
    memset(capture, 0, sizeof(*capture));
    capture->started_us = capture->edge_us = now;
    capture->phase = DHT11_CAPTURE_WAIT_RELEASE;
}

void DHT11Capture_Edge(DHT11Capture *capture, uint16_t now, bool high) {
    if (capture->phase == DHT11_CAPTURE_DONE || capture->phase == DHT11_CAPTURE_FAILED)
        return;
    if (capture->phase == DHT11_CAPTURE_WAIT_RELEASE) {
        if (!high) { fail(capture, DHT11_CAPTURE_SEQUENCE); return; }
        /* Timestamp the actual host release IRQ. A task may be preempted
           between arming EXTI and releasing PG6; that is harmless. */
        capture->started_us = capture->edge_us = now;
        capture->phase = DHT11_CAPTURE_WAIT_RESPONSE;
        return;
    }

    uint16_t width = (uint16_t)(now - capture->edge_us);
    capture->response_us = (uint16_t)(now - capture->started_us);
    if (capture->response_us > RESPONSE_LIMIT_US) {
        fail(capture, DHT11_CAPTURE_TIMEOUT);
        return;
    }
    bool expect_high = capture->phase == DHT11_CAPTURE_RESPONSE_LOW ||
                       capture->phase == DHT11_CAPTURE_DATA_LOW;
    if (high != expect_high) { fail(capture, DHT11_CAPTURE_SEQUENCE); return; }

    switch (capture->phase) {
    case DHT11_CAPTURE_WAIT_RESPONSE:
        if (width > 120U) { fail(capture, DHT11_CAPTURE_PULSE); return; }
        capture->phase = DHT11_CAPTURE_RESPONSE_LOW;
        break;
    case DHT11_CAPTURE_RESPONSE_LOW:
    case DHT11_CAPTURE_RESPONSE_HIGH:
        if (width < 50U || width > 110U) { fail(capture, DHT11_CAPTURE_PULSE); return; }
        capture->phase = capture->phase == DHT11_CAPTURE_RESPONSE_LOW ?
                         DHT11_CAPTURE_RESPONSE_HIGH : DHT11_CAPTURE_DATA_LOW;
        break;
    case DHT11_CAPTURE_DATA_LOW:
        if (width < 30U || width > 85U) { fail(capture, DHT11_CAPTURE_PULSE); return; }
        capture->phase = DHT11_CAPTURE_DATA_HIGH;
        break;
    case DHT11_CAPTURE_DATA_HIGH: {
        /* Separate the nominal 26/70 us pulses, rejecting the ambiguous band.
           EXTI priority 3 keeps audio/display IRQ latency out of these widths. */
        bool one = width >= 55U && width <= 100U;
        bool zero = width >= 10U && width <= 45U;
        if (!one && !zero) { fail(capture, DHT11_CAPTURE_PULSE); return; }
        unsigned byte = capture->bits / 8U;
        capture->data[byte] = (uint8_t)((capture->data[byte] << 1U) | (one ? 1U : 0U));
        capture->bits++;
        capture->phase = capture->bits == 40U ? DHT11_CAPTURE_DONE : DHT11_CAPTURE_DATA_LOW;
        break;
    }
    default:
        fail(capture, DHT11_CAPTURE_SEQUENCE);
        return;
    }
    capture->edge_us = now;
}

void DHT11Capture_Timeout(DHT11Capture *capture, uint16_t now) {
    if (capture->phase == DHT11_CAPTURE_DONE || capture->phase == DHT11_CAPTURE_FAILED)
        return;
    capture->response_us = (uint16_t)(now - capture->started_us);
    fail(capture, DHT11_CAPTURE_TIMEOUT);
}
