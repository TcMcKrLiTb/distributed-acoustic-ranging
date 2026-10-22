#include "sync_app.h"
#include "sync_port.h"
#include "Port/sync_log.h"
#include "cmsis_os.h"
#include "task.h"

static TimeSyncStatus published;
static bool published_valid;

static void publish_status(void) {
    TimeSyncStatus status;
    SyncApp_GetStatus(&status);
    status.log_dropped = SyncLog_Dropped();
    taskENTER_CRITICAL();
    published = status;
    published_valid = true;
    taskEXIT_CRITICAL();
}

void Sync_Task(void const *argument) {
    (void)argument;
    SyncLog_Init();
    SyncApp_Init();
    publish_status();
    for (;;) {
        SyncApp_Poll();
        publish_status();
        osDelay(1);
    }
}

bool TimeSync_GetStatus(TimeSyncStatus *status) {
    bool valid;
    if (!status) return false;
    taskENTER_CRITICAL();
    *status = published;
    valid = published_valid;
    taskEXIT_CRITICAL();
    return valid;
}

bool TimeSync_NowNs(uint64_t *time_ns) {
    bool ready;
    if (!time_ns) return false;
    taskENTER_CRITICAL();
    ready = published_valid && published.ready;
    if (ready) *time_ns = SyncPort_Time();
    taskEXIT_CRITICAL();
    return ready;
}

bool TimeSync_CaptureClockFromISR(uint64_t *time_ns, uint32_t *generation) {
    if (!time_ns || !generation || !published_valid || !published.ready) return false;
    return SyncPort_CaptureClock(time_ns, generation);
}

void TimeSync_ProcessLogs(void) {
    SyncLog_Poll();
}
