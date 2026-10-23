#ifndef APP_SYNC_LOG_H__
#define APP_SYNC_LOG_H__

#include <stdint.h>

/* Default: event-focused output. Set to 1 for periodic diagnostic statistics. */
#ifndef APP_LOG_VERBOSE
#define APP_LOG_VERBOSE 0
#endif

void SyncLog_Init(void);
/* Task-context producers (sync/audio). Full queue drops without waiting. */
void SyncLog_Write(const char *text);
/* Single consumer: the default task. */
void SyncLog_Poll(void);
uint32_t SyncLog_Dropped(void);

#endif
