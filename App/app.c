#include "app.h"
#include "TimeSync/time_sync.h"
#include "AudioArrival/audio_arrival.h"
#include "Ranging/ranging.h"
#include "DHT11/dht11.h"
#include "Diagnostics/pcm_dump.h"

/**
  * @brief  Initialize app part
  * @retval None
  */
void App_Init(void) {
    PcmDump_Init();
    AudioArrival_Init();
    Ranging_Init();
    DHT11_Init();
}

void App_BackgroundPoll(void) {
    TimeSync_ProcessLogs();
    PcmDump_Poll();
}
