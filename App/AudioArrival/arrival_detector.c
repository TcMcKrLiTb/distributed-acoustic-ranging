#include "arrival_detector.h"
#include <string.h>

/* Counts use the nominal 48 kHz rate; timestamp conversion uses the real clock. */
#define WARMUP_SAMPLES 48000U
#define REFRACTORY_SAMPLES 28800U
#define QUIET_SAMPLES 4800U
#define MIN_THRESHOLD 128U

void ArrivalDetector_Reset(ArrivalDetector *d) {
    memset(d, 0, sizeof(*d));
    d->noise_q8 = 16U * 256U;
    d->warmup = WARMUP_SAMPLES;
}

bool ArrivalDetector_Armed(const ArrivalDetector *d) {
    return !d->warmup && !d->blocked;
}

bool ArrivalDetector_Push(ArrivalDetector *d, int16_t pcm,
                          uint64_t sample, ArrivalDetection *event) {
    int32_t filtered = (int32_t)pcm - d->previous_input + d->highpass * 63 / 64;
    d->previous_input = pcm;
    d->highpass = filtered;
    uint32_t magnitude = (uint32_t)(filtered < 0 ? -filtered : filtered);
    uint32_t noise = d->noise_q8 / 256U;
    uint32_t threshold = noise * 8U;
    if (threshold < MIN_THRESHOLD) threshold = MIN_THRESHOLD;
    bool detected = false;

    if (d->warmup) {
        d->warmup--;
        d->noise_q8 = (uint32_t)((int32_t)d->noise_q8 +
            ((int32_t)(magnitude * 256U) - (int32_t)d->noise_q8) / 1024);
    } else if (d->blocked) {
        if (d->refractory) d->refractory--;
        if (magnitude < threshold / 2U) d->quiet++;
        else d->quiet = 0;
        if (!d->refractory && d->quiet >= QUIET_SAMPLES) d->blocked = false;
    } else if (d->candidate) {
        d->candidate_age++;
        if (magnitude > d->peak) d->peak = magnitude;
        if (magnitude >= d->threshold) d->hits++;
        int32_t sign = filtered >= 0 ? 1 : -1;

        if (sign != d->previous_sign) d->crossings++;
        d->previous_sign = sign;
        if (magnitude < d->threshold / 2U) d->candidate_quiet++;
        else d->candidate_quiet = 0;
        if (d->candidate_age >= ARRIVAL_CONFIRM_MIN_SAMPLES) {
            if (d->hits >= 8U && d->crossings >= 2U &&
                d->peak >= d->threshold * 2U) {
                /* Preserve the original 96-sample acceptance, even for a
                   strong transient followed by silence within that window. */
                d->onset.peak = d->peak;
                d->onset.confirmation_samples = d->candidate_age;
                *event = d->onset;
                detected = true;
                d->blocked = true;
                d->refractory = REFRACTORY_SAMPLES;
                d->quiet = 0;
                d->candidate = false;
            } else if (d->hits < 8U || d->crossings < 2U ||
                       d->candidate_quiet >= ARRIVAL_CANDIDATE_QUIET_SAMPLES ||
                       d->candidate_age >= ARRIVAL_CONFIRM_MAX_SAMPLES) {
                d->candidate = false;
            }
            /* A credible, continuous weak candidate retains its original
               crossing and frozen threshold for at most 10 ms. */
        }
    } else if (magnitude >= threshold && d->previous_abs < threshold) {
        d->candidate = true;
        d->candidate_age = 0;
        d->candidate_quiet = 0;
        d->hits = 1;
        d->crossings = 0;
        d->peak = magnitude;
        d->threshold = threshold;
        d->previous_sign = filtered >= 0 ? 1 : -1;
        d->onset.sample = sample ? sample - 1U : 0;
        uint32_t rise = magnitude - d->previous_abs;
        uint32_t fraction = ((threshold - d->previous_abs) * 65536ULL) / rise;
        if (fraction >= 65536U) {
            d->onset.sample = sample;
            fraction = 0;
        }
        d->onset.fraction_q16 = (uint16_t)fraction;
        d->onset.noise = noise;
        d->onset.threshold = threshold;
    } else {
        d->noise_q8 = (uint32_t)((int32_t)d->noise_q8 +
            ((int32_t)(magnitude * 256U) - (int32_t)d->noise_q8) / 1024);
    }
    d->previous_abs = magnitude;
    return detected;
}
