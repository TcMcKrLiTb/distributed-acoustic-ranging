#ifndef APP_ARRIVAL_CLOCK_H
#define APP_ARRIVAL_CLOCK_H
#include <stdint.h>
#define ARRIVAL_CLOCK_WINDOW 64U
typedef struct {
    uint64_t anchors[ARRIVAL_CLOCK_WINDOW];
    unsigned count, next;
} ArrivalClock;
void ArrivalClock_Reset(ArrivalClock *clock);
/* Task-side least squares over contiguous 256-frame DMA boundaries. Reduces
   frame-phase/IRQ capture jitter; does not remove fixed codec/FIFO delay. */
void ArrivalClock_Push(ArrivalClock *clock, uint64_t raw_last_ns, uint64_t nominal_period_ps,
                       uint64_t *last_ns, uint64_t *period_ps);
#endif
