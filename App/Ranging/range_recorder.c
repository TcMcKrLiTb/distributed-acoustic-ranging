#include "range_recorder.h"
#include <string.h>

void RangeRecorder_Reset(RangeRecorder *r) {
    r->available = 0; r->pending = false; r->next_sample = 0;
}
void RangeRecorder_Push(RangeRecorder *r, uint64_t sample, int16_t pcm) {
    if (r->available && sample != r->next_sample) RangeRecorder_Reset(r);
    r->history[sample % RANGE_HISTORY_SAMPLES] = pcm;
    r->next_sample = sample + 1U;
    if (r->available < RANGE_HISTORY_SAMPLES) r->available++;
}
bool RangeRecorder_Arm(RangeRecorder *r, uint32_t sequence, uint32_t generation,
                       uint64_t sample, uint16_t fraction, uint64_t onset_ns,
                       uint64_t period_ps, uint32_t flags) {
    if (r->pending || sample < RANGE_PRE_SAMPLES || sample >= r->next_sample ||
        period_ps < 20000000ULL || period_ps > 22000000ULL) return false;
    uint64_t first = sample - RANGE_PRE_SAMPLES;
    if (r->next_sample - first > r->available) return false;
    uint64_t before_ns = ((uint64_t)RANGE_PRE_SAMPLES * 65536ULL + fraction) * period_ps / (65536ULL * 1000ULL);
    if (onset_ns < before_ns) return false;
    r->sequence = sequence; r->generation = generation; r->flags = flags;
    r->onset_ns = onset_ns; r->start_ns = onset_ns - before_ns;
    r->period_ps = period_ps; r->first_sample = first; r->pending = true;
    return true;
}
bool RangeRecorder_Ready(RangeRecorder *r) {
    if (!r->pending) return false;
    if (r->next_sample - r->first_sample > RANGE_HISTORY_SAMPLES) { r->pending = false; return false; }
    return r->next_sample - r->first_sample >= RANGE_SAMPLES;
}
bool RangeRecorder_Copy(RangeRecorder *r, RangeCapture *c) {
    if (!c || !RangeRecorder_Ready(r)) return false;
    c->sequence = r->sequence; c->generation = r->generation; c->flags = r->flags;
    c->onset_ns = r->onset_ns; c->start_ns = r->start_ns; c->period_ps = r->period_ps;
    for (unsigned i = 0; i < RANGE_SAMPLES; ++i) {
        int16_t x = r->history[(r->first_sample + i) % RANGE_HISTORY_SAMPLES];
        c->pcm[i] = x;
        if (x >= 32760 || x <= -32760) c->flags |= RANGE_CAPTURE_CLIPPED;
    }
    r->pending = false;
    return true;
}
