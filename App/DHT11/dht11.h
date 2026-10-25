#ifndef APP_DHT11_H
#define APP_DHT11_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t temperature_c_x10;
    uint8_t humidity_percent;
    uint16_t temperature_raw, humidity_raw;
    uint32_t sampled_ms, attempts, samples, failures, response_us;
    uint8_t last_error; /* LibDriver result; 5 = port/timing error, 6 = not ready. */
    bool initialized, valid, timing_error;
} DHT11Status;

/* Call once from App_Init after MX_GPIO_Init/MX_TIM14_Init, before the scheduler.
   Creates a static low-priority task. The sensor is probed after 2 s warm-up. */
void DHT11_Init(void);

/* Task context only. Always copies diagnostics; true means the latest attempt
   produced a valid sample no older than 6 s. Values survive errors for diagnosis.
   Read this cache instead of accessing the single-owner LibDriver handle. */
bool DHT11_GetLatest(DHT11Status *status);

#ifdef __cplusplus
}
#endif
#endif
