#ifndef APP_RANGE_CONFIG_H
#define APP_RANGE_CONFIG_H
/* Reference at 20 C; runtime temperature correction preserves this value. */
#define RANGE_SOUND_SPEED_MM_S 343420U
/* Signed B-input delay minus A-input delay. A is the lower-UID master.
   Leave zero until independently measured; never fit each test distance. */
#define RANGE_RELATIVE_BIAS_NS 0
#endif
