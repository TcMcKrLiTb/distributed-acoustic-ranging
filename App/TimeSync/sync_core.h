#ifndef APP_SYNC_CORE_H__
#define APP_SYNC_CORE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNC_WIRE_SIZE 56U
#define SYNC_ETHERTYPE 0x88B5U /* IEEE local experimental EtherType; private protocol. */
#define SYNC_FLAG_LOCKED 1U
#define SYNC_FLAG_SAMPLE_VALID 2U
#define SYNC_LOCK_LIMIT_NS 2500LL
#define SYNC_LOCK_SAMPLES 16U
#define SYNC_MAX_FREQ_PPB 250000

typedef enum {
    SYNC_HELLO = 1,
    SYNC_EVENT,
    SYNC_FOLLOW_UP,
    SYNC_DELAY_REQ,
    SYNC_DELAY_RESP
} SyncMessageType;

typedef struct {
    uint8_t type, uid[12];
    uint16_t flags;
    uint32_t sequence, session;
    uint64_t timestamp_ns;
    int64_t offset_ns;
    uint32_t delay_ns;
    int32_t frequency_ppb;
    uint32_t samples;
} SyncMessage;

typedef struct {
    int64_t offset_ns, integral_ppb;
    int32_t frequency_ppb;
    uint32_t delay_ns, samples, rejected, good_run;
    uint32_t delay_window[16], delay_count, delay_index;
    bool locked, valid;
} SyncServo;

typedef struct {
    bool accepted, step;
    int64_t phase_ns;
    int32_t frequency_ppb;
} SyncCorrection;

void Sync_Encode(uint8_t out[SYNC_WIRE_SIZE], const SyncMessage *message);
bool Sync_Decode(SyncMessage *message, const uint8_t *data, size_t length);
void SyncServo_Reset(SyncServo *servo);
SyncCorrection SyncServo_Update(SyncServo *servo, uint64_t t1, uint64_t t2,
                               uint64_t t3, uint64_t t4);

#endif
