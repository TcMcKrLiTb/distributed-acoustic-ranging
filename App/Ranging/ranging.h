#ifndef APP_RANGING_H
#define APP_RANGING_H
#include "range_core.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RANGING_WAIT_SYNC, RANGING_WAIT_SOUND, RANGING_TRANSFER,
    RANGING_COMPUTING, RANGING_WAIT_DISPLAY
} RangingState;
typedef struct {
    RangingState state;
    bool master, locked, valid, source_a;
    uint32_t sequence, distance_mm, quality_permille, accepted, rejected;
    int64_t delay_ns;
    RangeError last_error;
} RangingStatus;

/* A-only DSP snapshot for the latest paired attempt, including rejected peaks.
   FRONT is diagnostic only; LONG alone drives the normal result protocol. */
typedef struct {
    bool valid;
    uint32_t a_sequence, b_sequence, completed_ms, dsp_ms;
    RangeEstimate long_window, front_window;
} RangingDiagnostics;

void Ranging_Init(void);
bool Ranging_GetStatus(RangingStatus *status);
bool Ranging_GetDiagnostics(RangingDiagnostics *diagnostics);
/* The following functions belong exclusively to SyncTask, including ETH TX. */
void Ranging_SetLink(bool master, bool locked, uint32_t session, uint32_t peer_session);
void Ranging_Receive(const uint8_t payload[RANGE_WIRE_SIZE]);
void Ranging_Poll(void);
#ifdef __cplusplus
}
#endif
#endif
