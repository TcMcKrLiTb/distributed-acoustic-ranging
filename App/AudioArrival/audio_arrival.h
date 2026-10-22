#ifndef APP_AUDIO_ARRIVAL_H
#define APP_AUDIO_ARRIVAL_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sequence, clock_generation;
    uint64_t time_ns, sample, interval_ns;
    uint32_t peak, noise, threshold;
    bool synchronized, clipped, interval_valid;
} AudioArrivalEvent;

/* Initializes the codec before GUI/RTOS tasks can use the shared I2C bus;
   creates a static task. Acquisition starts when the local PTP clock is ready. */
void AudioArrival_Init(void);
/* Task-context snapshot of the latest accepted event. */
bool AudioArrival_GetLatest(AudioArrivalEvent *event);

void audio_task(void const *argument);

#ifdef __cplusplus
}
#endif
#endif
