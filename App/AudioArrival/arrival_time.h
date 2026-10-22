#ifndef APP_ARRIVAL_TIME_H
#define APP_ARRIVAL_TIME_H
#include <stdbool.h>
#include <stdint.h>

/* IRQ can run late: subtract the frames DMA has received since the boundary.
   Reject a boundary once DMA has returned to writing that half of the ring. */
bool ArrivalTime_Boundary(uint64_t now_ns, uint32_t remaining_words,
                          unsigned half, uint64_t period_ps, uint64_t *last_ns);
/* Anchor is the last sample of a 256-frame block. Signed offset permits an
   onset carried from the preceding block during confirmation. */
bool ArrivalTime_At(uint64_t last_ns, int64_t relative_sample,
                    uint16_t fraction_q16, uint64_t period_ps, uint64_t *time_ns);
/* Difference between two accepted detections on one continuous PTP clock.
   This is a measured detection interval, not an independent source reference. */
bool ArrivalTime_Interval(bool have_previous, uint64_t previous_ns, uint32_t previous_epoch,
                          uint64_t current_ns, uint32_t current_epoch, uint64_t *interval_ns);
#define ARRIVAL_BLOCK_FRAMES 256U
#define ARRIVAL_DMA_WORDS (ARRIVAL_BLOCK_FRAMES * 4U)
#endif
