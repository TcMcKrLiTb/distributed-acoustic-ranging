#include "arrival_clock.h"
#include "arrival_time.h"

void ArrivalClock_Reset(ArrivalClock *c) { c->count = 0; c->next = 0; }
void ArrivalClock_Push(ArrivalClock *c, uint64_t raw, uint64_t nominal,
                       uint64_t *last, uint64_t *period) {
    *last = raw; *period = nominal;
    if (!nominal || nominal > 1000000000ULL) { ArrivalClock_Reset(c); return; }
    c->anchors[c->next] = raw;
    c->next = (c->next + 1U) % ARRIVAL_CLOCK_WINDOW;
    if (c->count < ARRIVAL_CLOCK_WINDOW) c->count++;
    if (c->count < 8U) return;
    unsigned n = c->count, first = n == ARRIVAL_CLOCK_WINDOW ? c->next : 0;
    uint64_t origin = c->anchors[first];
    int64_t sx = 0, sxx = 0, sy = 0, sxy = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t anchor = c->anchors[(first + i) % ARRIVAL_CLOCK_WINDOW];
        if (anchor < origin || anchor - origin > 1000000000ULL) { ArrivalClock_Reset(c); return; }
        int64_t residual = (int64_t)(anchor - origin) * 1000LL - (int64_t)i * ARRIVAL_BLOCK_FRAMES * (int64_t)nominal;
        sx += i; sxx += (int64_t)i * i; sy += residual; sxy += (int64_t)i * residual;
    }
    int64_t denominator = (int64_t)n * sxx - sx * sx;
    int64_t slope = ((int64_t)n * sxy - sx * sy) / denominator; /* ps per block */
    int64_t estimate_period = (int64_t)nominal + slope / ARRIVAL_BLOCK_FRAMES;
    int64_t period_error = estimate_period - (int64_t)nominal;
    /* Bounds catch discontinuities, not normal oscillator/PTP servo drift. */
    if (period_error > (int64_t)nominal / 500 || period_error < -(int64_t)nominal / 500) {
        ArrivalClock_Reset(c); return;
    }
    int64_t residual_last = (2LL * sy + slope * (2LL * (n - 1U) * n - 2LL * sx)) / (2LL * n);
    int64_t from_origin = (int64_t)(n - 1U) * ARRIVAL_BLOCK_FRAMES * (int64_t)nominal + residual_last;
    if (from_origin < 0) { ArrivalClock_Reset(c); return; }
    uint64_t estimate = origin + (uint64_t)from_origin / 1000U;
    uint64_t error = estimate > raw ? estimate - raw : raw - estimate;
    if (error > 2U * nominal / 1000U) { ArrivalClock_Reset(c); return; }
    *last = estimate; *period = (uint64_t)estimate_period;
}
