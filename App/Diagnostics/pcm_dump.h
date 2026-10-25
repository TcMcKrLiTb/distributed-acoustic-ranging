#ifndef APP_PCM_DUMP_H
#define APP_PCM_DUMP_H
#include "Ranging/range_core.h"

/* Ranging tuning diagnostics: set to 0 to disable PCM export. */
#ifndef APP_PCM_DUMP
#define APP_PCM_DUMP 1
#endif

void PcmDump_Init(void);
/* AudioTask only: copy a completed capture without waiting for the UART. */
void PcmDump_Submit(const RangeCapture *capture);
/* Default task only, serialized with SyncLog_Poll on the same UART. */
void PcmDump_Poll(void);
#endif
