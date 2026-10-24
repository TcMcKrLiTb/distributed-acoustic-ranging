#ifndef APP_RANGE_RECORDER_H
#define APP_RANGE_RECORDER_H
#include "range_core.h"
#define RANGE_HISTORY_SAMPLES 4096U
typedef struct {
    int16_t history[RANGE_HISTORY_SAMPLES];
    uint64_t next_sample, first_sample;
    uint32_t available;
    bool pending;
    uint32_t sequence, generation, flags;
    uint64_t onset_ns, start_ns, period_ps;
} RangeRecorder;
void RangeRecorder_Reset(RangeRecorder *recorder);
void RangeRecorder_Push(RangeRecorder *recorder, uint64_t sample, int16_t pcm);
bool RangeRecorder_Arm(RangeRecorder *recorder, uint32_t sequence, uint32_t generation,
                       uint64_t sample, uint16_t fraction, uint64_t onset_ns,
                       uint64_t period_ps, uint32_t flags);
bool RangeRecorder_Ready(RangeRecorder *recorder);
bool RangeRecorder_Copy(RangeRecorder *recorder, RangeCapture *capture);
#endif
