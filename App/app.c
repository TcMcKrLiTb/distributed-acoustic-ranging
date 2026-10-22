#include "app.h"
#include "TimeSync/time_sync.h"
#include "AudioArrival/audio_arrival.h"

void App_Init(void) {
    AudioArrival_Init();
}

void App_BackgroundPoll(void) {
    TimeSync_ProcessLogs();
}
