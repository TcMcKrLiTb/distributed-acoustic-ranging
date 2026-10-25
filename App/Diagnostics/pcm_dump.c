#include "pcm_dump.h"
#if APP_PCM_DUMP
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "TimeSync/time_sync.h"
#include "TimeSync/sync_format.h"
#include "TimeSync/Port/sync_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
#define DUMP_SLOTS 2U
#define CHUNK_SAMPLES 64U
enum { FREE, WRITING, READY, READING };
typedef struct {
    RangeCapture capture;
    uint32_t id;
    char board;
    uint8_t state;
} DumpSlot;
static DumpSlot slots[DUMP_SLOTS];
static uint32_t next_id, dropped, reported_dropped;
static int active;
static unsigned phase, offset;
static uint32_t crc;
/* The default task has a small stack; keep UART formatting storage here. */
static char line[384];

static uint32_t crc_byte(uint32_t value, uint8_t byte) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        value = (value >> 1) ^ ((value & 1U) ? 0xEDB88320U : 0U);
    return value;
}
void PcmDump_Init(void) {
    memset(slots, 0, sizeof(slots));
    next_id = dropped = reported_dropped = 0;
    active = -1; phase = offset = 0;
}
void PcmDump_Submit(const RangeCapture *capture) {
    if (!capture) return;
    TimeSyncStatus sync;
    bool known = TimeSync_GetStatus(&sync);
    int slot = -1;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < DUMP_SLOTS; ++i)
        if (slots[i].state == FREE) { slot = (int)i; slots[i].state = WRITING; break; }
    if (slot < 0) dropped++;
    taskEXIT_CRITICAL();
    if (slot < 0) return;
    /* No large copies or formatting with interrupts disabled. */
    slots[slot].capture = *capture;
    slots[slot].id = ++next_id;
    slots[slot].board = !known ? '?' : sync.role == TIME_SYNC_MASTER ? 'A' :
                                      sync.role == TIME_SYNC_SLAVE ? 'B' : '?';
    taskENTER_CRITICAL();
    slots[slot].state = READY;
    taskEXIT_CRITICAL();
}
void PcmDump_Poll(void) {
    if (dropped != reported_dropped) {
        uint32_t count = dropped;
        char warning[48];
        snprintf(warning, sizeof(warning), "PCM WARN dropped=%" PRIu32 "\r\n", count);
        SyncLog_Write(warning);
        reported_dropped = count;
    }
    if (active < 0) {
        taskENTER_CRITICAL();
        for (unsigned i = 0; i < DUMP_SLOTS; ++i)
            if (slots[i].state == READY &&
                (active < 0 || (int32_t)(slots[i].id - slots[active].id) < 0)) active = (int)i;
        if (active >= 0) slots[active].state = READING;
        taskEXIT_CRITICAL();
        if (active < 0) return;
        phase = offset = 0; crc = 0xFFFFFFFFU;
    }
    const DumpSlot *slot = &slots[active];
    const RangeCapture *capture = &slot->capture;
    unsigned count = 0;
    if (phase == 0) {
        char start[SYNC_I64_TEXT_SIZE], onset[SYNC_I64_TEXT_SIZE], period[SYNC_I64_TEXT_SIZE];
        SyncFormat_I64(start, (int64_t)capture->start_ns);
        SyncFormat_I64(onset, (int64_t)capture->onset_ns);
        SyncFormat_I64(period, (int64_t)capture->period_ps);
        snprintf(line, sizeof(line), "PCM BEGIN v=1 id=%" PRIu32 " board=%c seq=%" PRIu32
                 " epoch=%" PRIu32 " count=%u fs=48000 start_ns=%s onset_ns=%s period_ps=%s flags=%" PRIu32 "\r\n",
                 slot->id, slot->board, capture->sequence, capture->generation,
                 (unsigned)RANGE_SAMPLES, start, onset, period, capture->flags);
    } else if (phase == 1) {
        count = RANGE_SAMPLES - offset;
        if (count > CHUNK_SAMPLES) count = CHUNK_SAMPLES;
        int used = snprintf(line, sizeof(line), "PCM DATA id=%" PRIu32 " offset=%u hex=", slot->id, offset);
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < count; ++i) {
            uint16_t sample = (uint16_t)capture->pcm[offset + i];
            // Wire bytes are signed PCM16 little-endian, independent of the host.
            for (unsigned shift = 0; shift < 16; shift += 8) {
                uint8_t byte = (uint8_t)(sample >> shift);
                line[used++] = hex[byte >> 4]; line[used++] = hex[byte & 15U];
            }
        }
        line[used++] = '\r'; line[used++] = '\n'; line[used] = 0;
    } else {
        snprintf(line, sizeof(line), "PCM END id=%" PRIu32 " crc32=%08" PRIx32 "\r\n",
                 slot->id, crc ^ 0xFFFFFFFFU);
    }
    /* One bounded block per poll. Audio/Sync tasks and their IRQs can preempt it. */
    HAL_StatusTypeDef result = HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)strlen(line), 20U);
    if (result != HAL_OK) {
        char warning[64];
        snprintf(warning, sizeof(warning), "PCM ABORT id=%" PRIu32 " reason=uart\r\n", slot->id);
        SyncLog_Write(warning);
    } else if (phase == 0) {
        phase = 1;
        return;
    } else if (phase == 1) {
        for (unsigned i = 0; i < count; ++i) {
            uint16_t sample = (uint16_t)capture->pcm[offset + i];
            crc = crc_byte(crc, (uint8_t)sample);
            crc = crc_byte(crc, (uint8_t)(sample >> 8));
        }
        offset += count;
        if (offset == RANGE_SAMPLES) phase = 2;
        return;
    }
    taskENTER_CRITICAL();
    slots[active].state = FREE;
    taskEXIT_CRITICAL();
    active = -1;
}
#else
void PcmDump_Init(void) {}
void PcmDump_Submit(const RangeCapture *capture) { (void)capture; }
void PcmDump_Poll(void) {}
#endif
