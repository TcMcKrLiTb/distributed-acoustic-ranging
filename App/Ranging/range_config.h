#ifndef APP_RANGE_CONFIG_H
#define APP_RANGE_CONFIG_H
/* 20 C using c = 331.3 + 0.606*T m/s. Common to both boards. */
#define RANGE_SOUND_SPEED_MM_S 343420U
/* Signed B-input delay minus A-input delay. A is the lower-UID master.
   Leave zero until independently measured; never fit each test distance. */
#define RANGE_RELATIVE_BIAS_NS 0
#endif
