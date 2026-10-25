#ifndef APP_DHT11_PORT_H
#define APP_DHT11_PORT_H

#include <stdbool.h>
#include <stdint.h>

bool DHT11_PortInit(void);
void DHT11_PortBegin(void);
/* Must follow EVERY LibDriver init/read, including failures. Releases PG6,
   returns timing diagnostics and resumes scheduling. Hardware IRQs stay enabled. */
bool DHT11_PortFinish(uint32_t *response_us);
const char *DHT11_PortDiagnostic(void);

#endif
