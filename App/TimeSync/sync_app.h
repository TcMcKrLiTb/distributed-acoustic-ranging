#ifndef APP_SYNC_APP_H__
#define APP_SYNC_APP_H__

#include "time_sync.h"
void SyncApp_Init(void);
void SyncApp_Poll(void);
/* Internal snapshot: only the owning sync task calls this function. */
void SyncApp_GetStatus(TimeSyncStatus *status);
/* SyncTask only: application payloads share the existing private L2 channel. */
bool SyncApp_SendApplication(const uint8_t payload[56]);

#endif
