#ifndef APP_DHT11_CAPTURE_H
#define APP_DHT11_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DHT11_CAPTURE_WAIT_RELEASE, DHT11_CAPTURE_WAIT_RESPONSE,
    DHT11_CAPTURE_RESPONSE_LOW, DHT11_CAPTURE_RESPONSE_HIGH,
    DHT11_CAPTURE_DATA_LOW, DHT11_CAPTURE_DATA_HIGH,
    DHT11_CAPTURE_DONE, DHT11_CAPTURE_FAILED
} DHT11CapturePhase;

typedef enum {
    DHT11_CAPTURE_OK, DHT11_CAPTURE_SEQUENCE,
    DHT11_CAPTURE_PULSE, DHT11_CAPTURE_TIMEOUT
} DHT11CaptureError;

typedef struct {
    uint8_t data[5], bits;
    uint16_t started_us, edge_us, response_us;
    DHT11CapturePhase phase;
    DHT11CaptureError error;
} DHT11Capture;

/* Pure timestamp decoder. All differences support the 16-bit timer wrap.
   The first edge is the host releasing its open-drain start pulse. */
void DHT11Capture_Begin(DHT11Capture *capture, uint16_t now);
void DHT11Capture_Edge(DHT11Capture *capture, uint16_t now, bool high);
void DHT11Capture_Timeout(DHT11Capture *capture, uint16_t now);

#endif
