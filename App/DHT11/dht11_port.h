#ifndef APP_DHT11_PORT_H
#define APP_DHT11_PORT_H

#include <stdbool.h>
#include <stdint.h>

bool DHT11_PortInit(void);
void DHT11_PortBegin(void);
/* Must follow EVERY LibDriver init/read, including failures. Releases PG6,
   disables the capture line, and returns timing diagnostics. */
bool DHT11_PortFinish(uint32_t *response_us);
const char *DHT11_PortDiagnostic(void);
uint8_t DHT11_PortEdges(void);
uint8_t dht11_interface_read_frame(uint8_t data[5]);

#endif
