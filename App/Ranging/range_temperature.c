#include "range_temperature.h"
#include "range_config.h"
#include <math.h>

uint32_t Range_TemperatureSoundSpeed(int16_t temperature_c_x10) {
    if (temperature_c_x10 < RANGE_TEMPERATURE_MIN_X10 ||
        temperature_c_x10 > RANGE_TEMPERATURE_MAX_X10) return 0;
    double kelvin = 273.15 + (double)temperature_c_x10 / 10.0;
    return (uint32_t)lround((double)RANGE_SOUND_SPEED_MM_S * sqrt(kelvin / 293.15));
}
