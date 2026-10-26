#ifndef APP_RANGE_CORE_H
#define APP_RANGE_CORE_H
#include <stdbool.h>
#include <stdint.h>

/* A fixed format on both boards. 42.7 ms, including 5.3 ms before onset. */
#define RANGE_SAMPLES 2048U
#define RANGE_PRE_SAMPLES 256U
#define RANGE_SEARCH_SAMPLES 96
#define RANGE_WIRE_SIZE 56U
#define RANGE_CHUNK_SAMPLES 16U
#define RANGE_PAIR_NS 20000000LL
#define RANGE_CAPTURE_LOCKED 1U
#define RANGE_CAPTURE_CLIPPED 2U
#define RANGE_MIN_MM 200U
#define RANGE_MAX_MM 2000U
/* Keep valid +/-10 mm measurements at either physical range endpoint. */
#define RANGE_EDGE_GUARD_MM 10U

typedef struct {
    uint32_t sequence, generation, flags;
    uint64_t onset_ns, start_ns, period_ps;
    int16_t pcm[RANGE_SAMPLES];
} RangeCapture;

typedef struct {
    uint32_t sound_speed_mm_s;
    int32_t bias_ns; /* B chain delay minus A chain delay; subtract before abs. */
} RangeConfig;

typedef enum {
    RANGE_OK = 0, RANGE_BAD_CAPTURE, RANGE_LOW_SIGNAL, RANGE_LOW_CORRELATION,
    RANGE_AMBIGUOUS, RANGE_OUT_OF_RANGE
} RangeError;

typedef struct {
    int64_t raw_delay_ns, delay_ns;
    uint32_t distance_mm, quality_permille;
    /* Diagnostic peak before fractional interpolation; valid after full search. */
    int32_t peak_lag_samples;
    bool correlation_valid;
    bool source_a;
    RangeError error;
} RangeEstimate;

/* Pure DSP; run below the audio and sync tasks. No HAL or RTOS dependencies. */
bool Range_Estimate(const RangeCapture *a, const RangeCapture *b,
                    const RangeConfig *config, RangeEstimate *result);
bool Range_Convert(int64_t raw_ns, const RangeConfig *config, RangeEstimate *result);

typedef enum {
    RANGE_META = 1, RANGE_DATA, RANGE_ACK, RANGE_RESULT, RANGE_RESULT_ACK, RANGE_REJECT,
    RANGE_CONFIG_REQUEST, RANGE_CONFIG_STATE, RANGE_CONFIG_ACK, RANGE_SENSOR_UPDATE
} RangePacketType;
typedef struct {
    uint8_t type;
    uint32_t session, sequence;
    uint64_t onset_ns, start_ns, period_ps, apply_ns;
    int64_t delay_ns;
    uint32_t generation, flags, distance_mm, quality_permille, a_sequence, b_sequence;
    uint16_t offset, count;
    int16_t pcm[RANGE_CHUNK_SAMPLES];
    /* META/RESULT carry revision at bytes 52..55. Configuration messages use
       header sequence as request ID or master revision and signed C*10 at 16.
       STATE: request ACK at 20, cached-result flag at 24, corrected delay at 28,
       result ID at 36 and quality at 40. Cache imports never count as events. */
    int16_t temperature_c_x10;
    uint32_t config_revision, request_ack;
    /* Config and SENSOR_UPDATE: source at 18 (manual/A/B = 0/1/2), presence
       at 19 and ownership epoch at 44. SENSOR_UPDATE sequence is an attempt ID. */
    uint8_t temperature_source;
    bool sensor_present;
    uint32_t source_epoch;
} RangePacket;

bool Range_IsPacket(const uint8_t data[RANGE_WIRE_SIZE]);
bool Range_Decode(RangePacket *packet, const uint8_t data[RANGE_WIRE_SIZE]);
void Range_Encode(uint8_t data[RANGE_WIRE_SIZE], const RangePacket *packet);
#endif
