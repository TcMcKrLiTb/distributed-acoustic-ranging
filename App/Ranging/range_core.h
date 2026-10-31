#ifndef APP_RANGE_CORE_H
#define APP_RANGE_CORE_H
#include <stdbool.h>
#include <stdint.h>

/* A fixed format on both boards. 42.7 ms, including 5.3 ms before onset. */
#define RANGE_SAMPLES 2048U
#define RANGE_PRE_SAMPLES 256U
/* Maximum symmetric search supported by the fixed LONG/FRONT windows at the
   nominal rate. Each B interpolation still needs its following sample. */
#define RANGE_SEARCH_SAMPLES 127
#define RANGE_LONG_BEGIN 128U
#define RANGE_LONG_END (RANGE_SAMPLES - 128U)
#define RANGE_FRONT_BEGIN 128U
#define RANGE_FRONT_END 384U
/* Two nearby direct-sound windows end 4.0/4.67 ms after the coarse onset.
   The longer capture is still searched to corroborate their selected phase. */
#define RANGE_DIRECT_CHECK_END 448U
#define RANGE_DIRECT_END 480U
#define RANGE_DIRECT_MIN_QUALITY 850U
#define RANGE_SUPPORT_MIN_QUALITY 500U
#define RANGE_SUPPORT_MIN_MARGIN 21
#define RANGE_LATE_LIMIT 1024U
#define RANGE_LATE_SUPPORT_MIN 768U
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
    /* Diagnostic candidates remain separate from an accepted measurement. */
    uint16_t window_begin, window_end; /* end is exclusive */
    uint32_t competitor_quality_permille;
    int32_t competitor_lag_samples, peak_margin_permille;
    bool competitor_valid;
    float peak_fraction_samples;
    bool candidate_valid;
    int64_t candidate_raw_delay_ns;
    uint32_t candidate_distance_mm;
} RangeEstimate;

/* Pure DSP; run below the audio and sync tasks. No HAL or RTOS dependencies. */
bool Range_Estimate(const RangeCapture *a, const RangeCapture *b,
                    const RangeConfig *config, RangeEstimate *result);
bool Range_EstimateWindow(const RangeCapture *a, const RangeCapture *b,
                         const RangeConfig *config, unsigned begin, unsigned end,
                         RangeEstimate *result);
/* Accepted result requires both early windows and consistent LONG support.
   A rejected weak precursor may be retried around a distinct later burst on
   both boards. All reads remain within the original saved captures; returned
   window bounds describe the actual searched spans, with no padded samples.
   LONG and FRONT estimators above retain their original diagnostic behavior.
   long_result may be NULL, otherwise it must not alias result. */
bool Range_EstimateDirect(const RangeCapture *a, const RangeCapture *b,
                         const RangeConfig *config, RangeEstimate *result,
                         RangeEstimate *long_result);
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
