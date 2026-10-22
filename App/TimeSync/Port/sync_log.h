#ifndef APP_SYNC_LOG_H__
#define APP_SYNC_LOG_H__

#include <stdint.h>

void SyncLog_Init(void);
/* Task-context producers (sync/audio). Full queue drops without waiting. */
void SyncLog_Write(const char *text);
/* Single consumer: the default task. */
void SyncLog_Poll(void);
uint32_t SyncLog_Dropped(void);

#endif
