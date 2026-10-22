#include "arrival_time.h"
#include <limits.h>

bool ArrivalTime_Boundary(uint64_t now, uint32_t remaining, unsigned half,
                          uint64_t period_ps, uint64_t *last) {
    if (!last || !period_ps || remaining > ARRIVAL_DMA_WORDS || half > 1U) return false;
    uint32_t position = (ARRIVAL_DMA_WORDS - remaining) % ARRIVAL_DMA_WORDS;
    uint32_t boundary = half == 0U ? ARRIVAL_DMA_WORDS / 2U : 0U;
    uint32_t past = (position + ARRIVAL_DMA_WORDS - boundary) % ARRIVAL_DMA_WORDS;
    if (past >= ARRIVAL_DMA_WORDS / 2U) return false;
    uint64_t delay = (past / 2U) * period_ps / 1000U;
    if (now < delay) return false;
    *last = now - delay;
    return true;
}

bool ArrivalTime_At(uint64_t last, int64_t relative, uint16_t fraction,
                    uint64_t period_ps, uint64_t *time) {
    if (!time || !period_ps || period_ps > 1000000000ULL ||
        relative < -65536 || relative >= ARRIVAL_BLOCK_FRAMES) return false;
    int64_t delta_q16 = (relative - (ARRIVAL_BLOCK_FRAMES - 1U)) * 65536LL + fraction;
    int64_t delta_ns = delta_q16 * (int64_t)period_ps / (65536LL * 1000LL);
    if (delta_ns > 0 || (uint64_t)(-delta_ns) > last) return false;
    *time = last - (uint64_t)(-delta_ns);
    return true;
}

bool ArrivalTime_Interval(bool have_previous, uint64_t previous, uint32_t previous_epoch,
                          uint64_t current, uint32_t current_epoch, uint64_t *interval) {
    if (!interval) return false;
    *interval = 0;
    if (!have_previous || previous_epoch != current_epoch || current <= previous) return false;
    *interval = current - previous;
    return true;
}
