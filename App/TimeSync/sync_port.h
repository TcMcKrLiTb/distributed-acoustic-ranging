#ifndef APP_SYNC_PORT_H__
#define APP_SYNC_PORT_H__

#include <stdbool.h>
#include <stdint.h>
#include "sync_core.h"

typedef struct {
    uint8_t source_mac[6], payload[SYNC_WIRE_SIZE];
    uint64_t timestamp_ns;
    bool timestamp_valid;
} SyncRxPacket;

bool SyncPort_Init(uint8_t uid[12], uint8_t mac[6]);
bool SyncPort_Link(void);
bool SyncPort_Receive(SyncRxPacket *packet);
bool SyncPort_Send(const uint8_t destination[6], const uint8_t payload[SYNC_WIRE_SIZE],
                   bool timestamp, uint64_t *transmit_ns);
bool SyncPort_Correct(int64_t phase_ns, int32_t frequency_ppb);
void SyncPort_PpsPoll(void);
void SyncPort_PpsStats(uint32_t *rises,uint32_t *falls,uint32_t *high_seen,uint32_t *low_seen,
                       uint32_t *missed);
uint64_t SyncPort_Time(void);
/* Bounded, read-only capture; false while a PTP phase update is in progress. */
bool SyncPort_CaptureClock(uint64_t *time_ns, uint32_t *generation);
uint32_t SyncPort_Millis(void);
uint32_t SyncPort_Session(void);
void SyncPort_Log(const char *text);
void SyncPort_Led(bool locked);
#endif
