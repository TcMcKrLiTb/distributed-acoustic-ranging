#ifndef APP_ARRIVAL_DETECTOR_H
#define APP_ARRIVAL_DETECTOR_H
#include <stdbool.h>
#include <stdint.h>

/* Nominal 48 kHz confirmation bounds; the peak/shape gates stay unchanged. */
#define ARRIVAL_CONFIRM_MIN_SAMPLES 96U
#define ARRIVAL_CONFIRM_MAX_SAMPLES 480U
#define ARRIVAL_CANDIDATE_QUIET_SAMPLES 48U
/* Ignore immediate repeats for 200 ms, then require 100 ms of quiet. A short
   unrelated event must not mask the next test sound 400 ms later. Sustained
   sound still cannot re-arm, even after the minimum holdoff has elapsed. */
#define ARRIVAL_REFRACTORY_SAMPLES 9600U
#define ARRIVAL_QUIET_SAMPLES 4800U

typedef struct {
    uint64_t sample;
    uint16_t fraction_q16;
    uint32_t peak, threshold, noise;
    uint32_t confirmation_samples; /* Elapsed samples from the retained crossing. */
} ArrivalDetection;

typedef struct {
    int32_t previous_input, highpass;
    uint32_t previous_abs, noise_q8, threshold, warmup, refractory;
    uint32_t quiet, candidate_age, candidate_quiet, hits, crossings, peak;
    int32_t previous_sign;
    bool candidate, blocked;
    ArrivalDetection onset;
} ArrivalDetector;

void ArrivalDetector_Reset(ArrivalDetector *detector);
/* Absolute PCM sample index, fixed channel; works across DMA block boundaries. */
bool ArrivalDetector_Push(ArrivalDetector *detector, int16_t pcm,
                          uint64_t sample, ArrivalDetection *event);
bool ArrivalDetector_Armed(const ArrivalDetector *detector);
#endif
