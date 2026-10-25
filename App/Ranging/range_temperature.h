#ifndef APP_RANGE_TEMPERATURE_H
#define APP_RANGE_TEMPERATURE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RANGE_TEMPERATURE_MIN_X10 (-200)
#define RANGE_TEMPERATURE_MAX_X10 500
#define RANGE_TEMPERATURE_DEFAULT_X10 200

/* Ideal-gas square-root temperature dependence, anchored at the existing
   20 C value of 343420 mm/s. Invalid input returns zero. No HAL/RTOS use. */
uint32_t Range_TemperatureSoundSpeed(int16_t temperature_c_x10);

#ifdef __cplusplus
}
#endif
#endif
