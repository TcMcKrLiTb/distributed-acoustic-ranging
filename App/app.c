#include "app.h"
#include "TimeSync/time_sync.h"

void App_BackgroundPoll(void) {
    TimeSync_ProcessLogs();
}
