#ifndef APP_TIME_SYNC_H__
#define APP_TIME_SYNC_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIME_SYNC_STARTING = 0,
    TIME_SYNC_ERROR,
    TIME_SYNC_LINK_DOWN,
    TIME_SYNC_WAIT_PEER,
    TIME_SYNC_ACQUIRING,
    TIME_SYNC_LOCKED_ESTIMATE
} TimeSyncState;

typedef enum {
    TIME_SYNC_DISCOVERY = 0,
    TIME_SYNC_MASTER,
    TIME_SYNC_SLAVE
} TimeSyncRole;

typedef struct {
    TimeSyncState state;
    TimeSyncRole role;
    bool ready, link_up, sample_valid, locked;
    uint8_t uid[12], mac[6];
    uint32_t uptime_ms;
    /* On the master these two values are reported by the slave's HELLO. */
    int64_t offset_ns;
    uint32_t delay_ns;
    /* Servo counters and frequency correction always describe this board. */
    int32_t frequency_ppb;
    uint32_t samples, rejected, tx_failures, rx_count;
    uint32_t pps_rises, pps_falls, pin_high_seen, pin_low_seen, pps_missed;
    uint32_t log_dropped;
} TimeSyncStatus;

/* Task-context API, also callable from C++/TouchGFX. Returns false until the
   sync task publishes its first snapshot. ERROR is a valid published state. */
bool TimeSync_GetStatus(TimeSyncStatus *status);
/* Local PTP time; readiness does not imply synchronization with another board.
   The time can jump during initial acquisition. Task context only. */
bool TimeSync_NowNs(uint64_t *time_ns);
/* Single consumer: call from the default task, never from the sync task/ISR. */
void TimeSync_ProcessLogs(void);

#ifdef __cplusplus
}
#endif
#endif
