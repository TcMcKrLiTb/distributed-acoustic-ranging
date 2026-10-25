#include "ranging.h"
#include "range_config.h"
#include "AudioArrival/audio_arrival.h"
#include "TimeSync/time_sync.h"
#include "TimeSync/sync_app.h"
#include "TimeSync/sync_format.h"
#include "TimeSync/Port/sync_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RETRY_MS 12U
#define TRANSACTION_MS 1500U
#define DISPLAY_LEAD_NS 250000000ULL
typedef enum { IDLE, META_TX, DATA_TX, DATA_RX, DSP, RESULT_TX, DISPLAY_WAIT, RESULT_WAIT } Stage;
typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE } JobState;

static RangeCapture local_capture, remote_capture;
static RangePacket result_packet;
static RangeEstimate job_result, job_front_result;
static uint32_t job_dsp_ms;
static bool job_success, discard_job;
static volatile JobState job_state;
static TaskHandle_t worker;
static StaticTask_t worker_control;
static StackType_t worker_stack[1536];
static RangingStatus status, published;
static RangingDiagnostics diagnostics, published_diagnostics;
static bool initialized, context_valid, local_valid, remote_valid, result_acked;
static bool announced;
static bool link_master, link_locked;
static uint32_t local_session, remote_session, active_generation, last_sequence;
static uint32_t started_ms, sent_ms, next_result_id;
static uint16_t transferred;
static Stage stage;
static const RangeConfig config = {RANGE_SOUND_SPEED_MM_S, RANGE_RELATIVE_BIAS_NS};

static void publish(void) {
    taskENTER_CRITICAL();
    published = status; published_diagnostics = diagnostics;
    taskEXIT_CRITICAL();
}
static void run_job(void) {
    TickType_t began = xTaskGetTickCount();
    /* Both calculations read the same immutable pair. Only LONG is submitted
       to the existing result state machine and Ethernet protocol. */
    job_success = Range_Estimate(&local_capture, &remote_capture, &config, &job_result);
    Range_EstimateWindow(&local_capture, &remote_capture, &config,
                         RANGE_FRONT_BEGIN, RANGE_FRONT_END, &job_front_result);
    job_dsp_ms = (uint32_t)(xTaskGetTickCount() - began) * portTICK_PERIOD_MS;
    taskENTER_CRITICAL(); job_state = JOB_DONE; taskEXIT_CRITICAL();
}
static void work(void *argument) {
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_job();
    }
}
void Ranging_Init(void) {
    status.state = RANGING_WAIT_SYNC;
    worker = xTaskCreateStatic(work, "RangeDSP", 1536, NULL, tskIDLE_PRIORITY + 2U,
                              worker_stack, &worker_control);
    initialized = worker != NULL;
    publish();
}
bool Ranging_GetStatus(RangingStatus *out) {
    if (!out) return false;
    taskENTER_CRITICAL(); *out = published; taskEXIT_CRITICAL();
    return initialized;
}
bool Ranging_GetDiagnostics(RangingDiagnostics *out) {
    if (!out) return false;
    taskENTER_CRITICAL(); *out = published_diagnostics; taskEXIT_CRITICAL();
    return initialized && out->valid;
}
static void clear_diagnostics(void) {
    memset(&diagnostics, 0, sizeof(diagnostics));
}
static void log_comparison(const char *window, const RangeEstimate *r, bool final) {
    char q[12] = "NA", q2[12] = "NA", gap[12] = "NA", lag[12] = "NA", frac[12] = "NA";
    char delay[SYNC_I64_TEXT_SIZE] = "NA", mm[12] = "NA", elapsed[32] = "";
    if (r->correlation_valid) {
        snprintf(q, sizeof(q), "%" PRIu32, r->quality_permille);
        snprintf(lag, sizeof(lag), "%" PRId32, r->peak_lag_samples);
        if (r->competitor_valid) {
            snprintf(q2, sizeof(q2), "%" PRIu32, r->competitor_quality_permille);
            snprintf(gap, sizeof(gap), "%" PRId32, r->peak_margin_permille);
        }
    }
    if (r->candidate_valid) {
        snprintf(frac, sizeof(frac), "%" PRId32,
                 (int32_t)lroundf(r->peak_fraction_samples * 65536.0f));
        SyncFormat_I64(delay, r->candidate_raw_delay_ns);
        snprintf(mm, sizeof(mm), "%" PRIu32, r->candidate_distance_mm);
    }
    if (final) snprintf(elapsed, sizeof(elapsed), " dsp_ms=%" PRIu32, job_dsp_ms);
    char line[256];
    snprintf(line, sizeof(line), "RANGE CMP a=%" PRIu32 " b=%" PRIu32
             " win=%s error=%u q=%s q2=%s gap=%s lag=%s frac=%s dt_ns=%s mm=%s%s\r\n",
             local_capture.sequence, remote_capture.sequence, window, (unsigned)r->error,
             q, q2, gap, lag, frac, delay, mm, elapsed);
    SyncLog_Write(line);
}
static void complete_diagnostics(void) {
    diagnostics.valid = true;
    diagnostics.a_sequence = local_capture.sequence;
    diagnostics.b_sequence = remote_capture.sequence;
    diagnostics.completed_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    diagnostics.dsp_ms = job_dsp_ms;
    diagnostics.long_window = job_result;
    diagnostics.front_window = job_front_result;
    log_comparison("LONG", &job_result, false);
    log_comparison("FRONT", &job_front_result, true);
}
static void reset_transaction(void) {
    stage = IDLE; local_valid = false; remote_valid = false; transferred = 0;
    result_acked = false;
    status.state = link_locked ? RANGING_WAIT_SOUND : RANGING_WAIT_SYNC;
}
void Ranging_SetLink(bool master, bool locked, uint32_t session, uint32_t peer_session) {
    bool changed = !context_valid || master != link_master || session != local_session ||
                   peer_session != remote_session || (link_locked && !locked);
    link_master = master; link_locked = locked;
    local_session = session; remote_session = peer_session; context_valid = true;
    status.master = master; status.locked = locked;
    if (changed) {
        discard_job = job_state != JOB_IDLE;
        reset_transaction(); status.valid = false;
        clear_diagnostics();
        active_generation = UINT32_MAX;
    }
    if (!locked) status.state = RANGING_WAIT_SYNC;
    else if (stage == IDLE && !remote_valid) status.state = RANGING_WAIT_SOUND;
}
static bool send_packet(RangePacket *p) {
    uint8_t bytes[RANGE_WIRE_SIZE];
    p->session = local_session;
    Range_Encode(bytes, p);
    return SyncApp_SendApplication(bytes);
}
static void acknowledge(uint32_t sequence) {
    RangePacket ack = {0}; ack.type = RANGE_ACK;
    ack.sequence = sequence; ack.offset = transferred;
    send_packet(&ack);
}
static void reject(const char *reason, RangeError error, const RangeEstimate *estimate) {
    RangePacket p = {0}; p.type = RANGE_REJECT;
    p.sequence = link_master ? remote_capture.sequence : local_capture.sequence;
    send_packet(&p);
    status.rejected++; status.last_error = error;
    char line[128];
    if (estimate && estimate->correlation_valid) {
        snprintf(line, sizeof(line), "RANGE REJECT reason=%s error=%u quality=%" PRIu32
                 " lag=%" PRId32 "\r\n", reason, (unsigned)error,
                 estimate->quality_permille, estimate->peak_lag_samples);
    } else {
        snprintf(line, sizeof(line), "RANGE REJECT reason=%s error=%u\r\n", reason, (unsigned)error);
    }
    SyncLog_Write(line);
    reset_transaction();
}
static bool paired(void) {
    if (!local_valid || !remote_valid) return false;
    uint64_t a = local_capture.onset_ns, b = remote_capture.onset_ns;
    return (a > b ? a - b : b - a) <= (uint64_t)RANGE_PAIR_NS;
}
static void accept_meta(const RangePacket *p) {
    remote_capture.sequence = p->sequence; remote_capture.generation = p->generation;
    remote_capture.flags = p->flags; remote_capture.onset_ns = p->onset_ns;
    remote_capture.start_ns = p->start_ns; remote_capture.period_ps = p->period_ps;
    remote_valid = true;
}
void Ranging_Receive(const uint8_t bytes[RANGE_WIRE_SIZE]) {
    RangePacket p;
    if (!initialized || !link_locked || discard_job || !Range_Decode(&p, bytes) || p.session != remote_session) return;
    uint64_t now; uint32_t generation;
    if (!TimeSync_CaptureClockFromISR(&now, &generation) || generation != active_generation) return;
    if (p.type == RANGE_REJECT) {
        if (stage != IDLE && stage != DSP && p.sequence ==
            (link_master ? remote_capture.sequence : local_capture.sequence)) {
            status.rejected++; reset_transaction();
        }
        return;
    }
    if (link_master) {
        if (p.type == RANGE_META) {
            if (stage == DSP || stage == RESULT_TX || stage == DISPLAY_WAIT) return;
            if ((p.onset_ns > now ? p.onset_ns - now : now - p.onset_ns) > 1000000000ULL) return;
            if (remote_valid && remote_capture.sequence != p.sequence && stage == DATA_RX) return;
            if (!remote_valid) {
                clear_diagnostics();
                accept_meta(&p); transferred = 0;
                if (!local_valid) started_ms = xTaskGetTickCount();
                status.state = RANGING_TRANSFER;
            }
            if (p.sequence == remote_capture.sequence && paired()) {
                stage = DATA_RX; acknowledge(p.sequence);
            }
        } else if (p.type == RANGE_DATA && remote_valid && p.sequence == remote_capture.sequence) {
            if (stage == DATA_RX && p.offset == transferred) {
                memcpy(remote_capture.pcm + transferred, p.pcm, p.count * sizeof(int16_t));
                transferred = (uint16_t)(transferred + p.count);
            }
            if (stage == DATA_RX || stage == DSP) acknowledge(p.sequence);
            if (stage == DATA_RX && transferred == RANGE_SAMPLES) {
                stage = DSP; status.state = RANGING_COMPUTING;
                taskENTER_CRITICAL(); job_state = JOB_RUNNING; taskEXIT_CRITICAL();
                xTaskNotifyGive(worker);
            }
        } else if (p.type == RANGE_RESULT_ACK && stage == RESULT_TX && p.sequence == result_packet.sequence) {
            result_acked = true;
        }
    } else {
        if (p.type == RANGE_ACK && local_valid && p.sequence == local_capture.sequence) {
            if (stage == META_TX && p.offset == 0) { stage = DATA_TX; transferred = 0; sent_ms = 0; }
            else if (stage == DATA_TX && p.offset == transferred + RANGE_CHUNK_SAMPLES) {
                transferred = p.offset; sent_ms = 0;
                if (transferred == RANGE_SAMPLES) stage = RESULT_WAIT;
            }
        } else if (p.type == RANGE_RESULT && local_valid && p.b_sequence == local_capture.sequence &&
                   p.generation == local_capture.generation && p.generation == generation &&
                   (stage == RESULT_WAIT || stage == DATA_TX || stage == DISPLAY_WAIT)) {
            /* A duplicate must neither move the scheduled display nor apply twice. */
            if (stage == DISPLAY_WAIT && p.sequence != result_packet.sequence) return;
            if (p.apply_ns <= now || p.apply_ns - now > 500000000ULL) return;
            result_packet = p; stage = DISPLAY_WAIT; status.state = RANGING_WAIT_DISPLAY;
            RangePacket ack = {0}; ack.type = RANGE_RESULT_ACK; ack.sequence = p.sequence;
            send_packet(&ack);
        }
    }
}
static void display_result(void) {
    status.valid = true; status.source_a = result_packet.delay_ns > 0;
    status.sequence = result_packet.sequence; status.distance_mm = result_packet.distance_mm;
    status.quality_permille = result_packet.quality_permille; status.delay_ns = result_packet.delay_ns;
    status.last_error = RANGE_OK; status.accepted++;
    char line[240];
#if APP_LOG_VERBOSE
    char delta[SYNC_I64_TEXT_SIZE];
    SyncFormat_I64(delta, status.delay_ns);
    snprintf(line, sizeof(line), "RANGE RESULT id=%" PRIu32 " board=%c distance_mm=%" PRIu32
        " source=%c dt_ns=%s quality=%" PRIu32 " a_seq=%" PRIu32 " b_seq=%" PRIu32 "\r\n",
        status.sequence, link_master ? 'A' : 'B', status.distance_mm, status.source_a ? 'A' : 'B',
        delta, status.quality_permille, result_packet.a_sequence, result_packet.b_sequence);
#else
    snprintf(line, sizeof(line), "RANGE RESULT id=%" PRIu32 " distance_mm=%" PRIu32
             " source=%c quality=%" PRIu32 "\r\n",
             status.sequence, status.distance_mm, status.source_a ? 'A' : 'B',
             status.quality_permille);
#endif
    SyncLog_Write(line);
    reset_transaction();
}
void Ranging_Poll(void) {
    if (!initialized) return;
    if (!announced) {
#if APP_LOG_VERBOSE
        char line[160];
        snprintf(line, sizeof(line), "RANGE INIT samples=2048 pre=256 c_mm_s=%" PRIu32 " bias_ns=%" PRId32
                 " range_mm=200..2000 method=normalized_xcorr\r\n", config.sound_speed_mm_s, config.bias_ns);
        SyncLog_Write(line);
#else
        SyncLog_Write("RANGE INIT range_mm=200..2000\r\n");
#endif
        char windows[128];
        snprintf(windows, sizeof(windows), "RANGE DSP primary=LONG compare=FRONT long=%u:%u front=%u:%u lag=-%d:%d\r\n",
                 (unsigned)RANGE_LONG_BEGIN, (unsigned)RANGE_LONG_END,
                 (unsigned)RANGE_FRONT_BEGIN, (unsigned)RANGE_FRONT_END,
                 RANGE_SEARCH_SAMPLES, RANGE_SEARCH_SAMPLES);
        SyncLog_Write(windows);
        announced = true;
    }
    uint64_t now; uint32_t generation;
    bool clock_ok = TimeSync_CaptureClockFromISR(&now, &generation);
    uint32_t ms = xTaskGetTickCount();
    if (job_state == JOB_DONE) {
        taskENTER_CRITICAL(); job_state = JOB_IDLE; taskEXIT_CRITICAL();
        if (discard_job || !link_locked || !clock_ok || generation != active_generation || stage != DSP) {
            discard_job = false; reset_transaction();
        } else {
            complete_diagnostics();
            if (!job_success) {
                reject("quality_or_range", job_result.error, &job_result);
            } else {
                memset(&result_packet, 0, sizeof(result_packet));
                result_packet.type = RANGE_RESULT; result_packet.sequence = ++next_result_id;
                result_packet.delay_ns = job_result.delay_ns; result_packet.distance_mm = job_result.distance_mm;
                result_packet.quality_permille = job_result.quality_permille;
                result_packet.a_sequence = local_capture.sequence; result_packet.b_sequence = remote_capture.sequence;
                result_packet.generation = remote_capture.generation; result_packet.apply_ns = now + DISPLAY_LEAD_NS;
                stage = RESULT_TX; status.state = RANGING_WAIT_DISPLAY; sent_ms = ms - RETRY_MS;
            }
        }
    }
    if (!clock_ok || !link_locked) { publish(); return; }
    if (generation != active_generation) {
        discard_job = job_state != JOB_IDLE; reset_transaction(); status.valid = false;
        clear_diagnostics();
        active_generation = generation;
    }
    if (discard_job || job_state != JOB_IDLE) { publish(); return; }

    /* Only copy into buffers while the worker and network no longer own them. */
    if (!local_valid && (stage == IDLE || (link_master && remote_valid))) {
        if (AudioArrival_TakeCapture(&local_capture)) {
            if (local_capture.sequence != last_sequence && local_capture.generation == generation &&
                local_capture.flags == RANGE_CAPTURE_LOCKED && now >= local_capture.onset_ns &&
                now - local_capture.onset_ns < 500000000ULL) {
                last_sequence = local_capture.sequence; local_valid = true;
                clear_diagnostics();
                if (!remote_valid) started_ms = ms;
                status.state = RANGING_TRANSFER;
                if (!link_master) { stage = META_TX; transferred = 0; sent_ms = ms - RETRY_MS; }
                else if (remote_valid && paired()) { stage = DATA_RX; acknowledge(remote_capture.sequence); }
            }
        }
    }
    if ((local_valid || remote_valid) && ms - started_ms > TRANSACTION_MS && stage != RESULT_TX && stage != DISPLAY_WAIT) {
        reject("timeout_or_unpaired", RANGE_BAD_CAPTURE, NULL);
    }
    if (ms - sent_ms >= RETRY_MS) {
        if (stage == META_TX) {
            RangePacket p = {0}; p.type = RANGE_META; p.sequence = local_capture.sequence;
            p.onset_ns = local_capture.onset_ns; p.start_ns = local_capture.start_ns;
            p.period_ps = local_capture.period_ps; p.generation = local_capture.generation; p.flags = local_capture.flags;
            send_packet(&p); sent_ms = ms;
        } else if (stage == DATA_TX) {
            RangePacket p = {0}; p.type = RANGE_DATA; p.sequence = local_capture.sequence;
            p.offset = transferred; p.count = RANGE_CHUNK_SAMPLES;
            memcpy(p.pcm, local_capture.pcm + transferred, sizeof(p.pcm));
            send_packet(&p); sent_ms = ms;
        } else if (stage == RESULT_TX && now < result_packet.apply_ns) {
            send_packet(&result_packet); sent_ms = ms;
        }
    }
    if ((stage == RESULT_TX || stage == DISPLAY_WAIT) && now >= result_packet.apply_ns) {
        if (stage == DISPLAY_WAIT || result_acked) display_result();
        else reject("result_ack_timeout", RANGE_BAD_CAPTURE, NULL);
    }
    publish();
}
