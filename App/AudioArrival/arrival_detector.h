#ifndef APP_ARRIVAL_DETECTOR_H
#define APP_ARRIVAL_DETECTOR_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t sample;
    uint16_t fraction_q16;
    uint32_t peak, threshold, noise;
} ArrivalDetection;

typedef struct {
    int32_t previous_input, highpass;
    uint32_t previous_abs, noise_q8, threshold, warmup, refractory;
    uint32_t quiet, candidate_age, hits, crossings, peak;
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
