"""Host integration checks for real ranging state machine and comparison logs.

Requires Python standard library and a native C compiler. Uses recorded PCM for
the historical rejection case when available; otherwise uses a deterministic
shared-front/independent-tail fixture. Does not
open serial ports, flash hardware, or substitute the production DSP/protocol.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib


HARNESS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
static int checked_snprintf(char *, size_t, const char *, ...);
/* Exercise the real formatting paths with a truncation/float-format guard. */
#define snprintf checked_snprintf
#include "Ranging/ranging.c"
#undef snprintf
#include "historical.h"

static uint64_t fake_now;
static uint32_t fake_ms, fake_epoch, tick_step;
static bool clock_ready, capture_ready;
static DHT11Status sensor_sample;
static RangeCapture offered_capture;
static RangePacket sent[2048];
static unsigned sent_count, notified;
static char logs[1024][384];
static unsigned log_count;

static void fail(const char *expr, unsigned line) {
    fprintf(stderr, "FAIL line %u: %s (stage=%d job=%d packets=%u)\n", line, expr, stage, job_state, sent_count);
    for (unsigned i = 0; i < log_count; ++i) fputs(logs[i], stderr);
    exit(1);
}
#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)
static int checked_snprintf(char *dst, size_t size, const char *format, ...) {
    for (const char *p = format; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0.123456789*hljztL", *p)) ++p;
        CHECK(*p && !strchr("aAeEfFgG", *p));
    }
    va_list args; va_start(args, format);
    int result = vsnprintf(dst, size, format, args);
    va_end(args);
    CHECK(result >= 0 && (size_t)result < size);
    return result;
}
TickType_t xTaskGetTickCount(void) { TickType_t value = fake_ms; fake_ms += tick_step; return value; }
uint32_t HAL_GetTick(void) { return fake_ms; }
bool DHT11_GetLatest(DHT11Status *out) { *out = sensor_sample; return out->valid; }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, uint32_t depth,
                              void *arg, unsigned priority, StackType_t *stack, StaticTask_t *control) {
    (void)fn; (void)name; (void)depth; (void)arg; (void)priority; (void)stack; (void)control;
    return (TaskHandle_t)(uintptr_t)1;
}
uint32_t ulTaskNotifyTake(int clear, uint32_t timeout) { (void)clear; (void)timeout; return 0; }
void xTaskNotifyGive(TaskHandle_t task) { CHECK(task != NULL); notified++; }
bool TimeSync_CaptureClockFromISR(uint64_t *now, uint32_t *epoch) {
    *now = fake_now; *epoch = fake_epoch; return clock_ready;
}
bool AudioArrival_TakeCapture(RangeCapture *out) {
    if (!capture_ready) return false;
    *out = offered_capture; capture_ready = false; return true;
}
bool SyncApp_SendApplication(const uint8_t payload[56]) {
    CHECK(sent_count < 2048);
    CHECK(Range_Decode(&sent[sent_count], payload));
    sent_count++; return true;
}
void SyncLog_Write(const char *line) {
    size_t n = strlen(line);
    CHECK(log_count < 1024 && n < sizeof(logs[0]));
    CHECK(n >= 2 && line[n-2] == '\r' && line[n-1] == '\n');
    memcpy(logs[log_count++], line, n+1);
}
static unsigned packets(unsigned type) {
    unsigned count = 0;
    for (unsigned i = 0; i < sent_count; ++i) if (sent[i].type == type) count++;
    return count;
}
static unsigned lines(const char *prefix) {
    unsigned count = 0;
    for (unsigned i = 0; i < log_count; ++i) if (!strncmp(logs[i], prefix, strlen(prefix))) count++;
    return count;
}
static void receive(RangePacket *packet) {
    uint8_t wire[RANGE_WIRE_SIZE]; packet->session = remote_session;
    Range_Encode(wire, packet); Ranging_Receive(wire);
}
static void advance(uint32_t ms) { fake_ms += ms; fake_now += (uint64_t)ms * 1000000ULL; }
static void reset(bool master_role, uint64_t now, uint32_t epoch) {
    memset(&local_capture, 0, sizeof(local_capture)); memset(&remote_capture, 0, sizeof(remote_capture));
    memset(&result_packet, 0, sizeof(result_packet)); memset(&job_result, 0, sizeof(job_result));
    memset(&job_front_result, 0, sizeof(job_front_result));
    memset(&status, 0, sizeof(status)); memset(&published, 0, sizeof(published));
    memset(&diagnostics, 0, sizeof(diagnostics)); memset(&published_diagnostics, 0, sizeof(published_diagnostics));
    job_dsp_ms = 0; job_success = discard_job = false; job_state = JOB_IDLE;
    initialized = context_valid = local_valid = remote_valid = result_acked = announced = false;
    link_master = link_locked = false;
    local_session = remote_session = active_generation = last_sequence = 0;
    started_ms = sent_ms = next_result_id = 0; transferred = 0; stage = IDLE;
    config = (RangeConfig){RANGE_SOUND_SPEED_MM_S, RANGE_RELATIVE_BIAS_NS};
    job_config = config; job_config_revision = 0;
    temperature = (RangingTemperatureStatus){.temperature_c_x10 = 200,
        .sound_speed_mm_s = RANGE_SOUND_SPEED_MM_S, .revision = 1};
    memset(&published_temperature, 0, sizeof(published_temperature));
    request_queued = local_request_pending = config_known = false;
    queued_temperature = requested_temperature = 0;
    queued_source = requested_source = temperature_source = TEMPERATURE_MANUAL;
    source_epoch = 1; requested_epoch = 0;
    next_request_id = pending_request_id = peer_request_ack = 0;
    config_sent_ms = request_sent_ms = sensor_sent_ms = sensor_refresh_ms = 0;
    peer_sensor_ms = peer_sensor_sequence = 0;
    owner_failed = have_accepted_delay = false; accepted_delay_ns = 0;
    memset(&sensor_sample, 0, sizeof(sensor_sample));
    fake_now = now; fake_ms = (uint32_t)(now / 1000000ULL); fake_epoch = epoch; tick_step = 0;
    clock_ready = true; capture_ready = false; sent_count = notified = log_count = 0;
    Ranging_Init(); Ranging_SetLink(master_role, true, 111, 222); Ranging_Poll();
    RangePacket initial = {0};
    initial.type = master_role ? RANGE_CONFIG_ACK : RANGE_CONFIG_STATE;
    initial.sequence = 1; initial.temperature_c_x10 = 200; initial.source_epoch = 1;
    receive(&initial); Ranging_Poll();
    CHECK(temperature.synchronized);
    CHECK(initialized && stage == IDLE && active_generation == epoch);
    CHECK(RANGE_SEARCH_SAMPLES == 127);
    CHECK(lines("RANGE DSP primary=LONG compare=FRONT ") == 1);
}
static void offer(const RangeCapture *capture) { offered_capture = *capture; capture_ready = true; }
static void metadata(const RangeCapture *capture) {
    RangePacket p = {0}; p.type = RANGE_META; p.sequence = capture->sequence;
    p.onset_ns = capture->onset_ns; p.start_ns = capture->start_ns; p.period_ps = capture->period_ps;
    p.generation = capture->generation; p.flags = capture->flags;
    p.config_revision = temperature.revision; receive(&p);
}
static void transfer_to_master(const RangeCapture *a, const RangeCapture *b) {
    unsigned first_ack = packets(RANGE_ACK), first_notify = notified;
    offer(a); Ranging_Poll(); CHECK(local_valid && !diagnostics.valid);
    metadata(b); CHECK(stage == DATA_RX && transferred == 0);
    for (unsigned offset = 0; offset < RANGE_SAMPLES; offset += RANGE_CHUNK_SAMPLES) {
        RangePacket p = {0}; p.type = RANGE_DATA; p.sequence = b->sequence;
        p.offset = (uint16_t)offset; p.count = RANGE_CHUNK_SAMPLES;
        memcpy(p.pcm, b->pcm + offset, sizeof(p.pcm)); receive(&p);
        CHECK(transferred == offset + RANGE_CHUNK_SAMPLES);
        CHECK(sent[sent_count-1].type == RANGE_ACK && sent[sent_count-1].offset == transferred);
    }
    CHECK(packets(RANGE_ACK) - first_ack == 129 && notified == first_notify + 1);
    CHECK(stage == DSP && job_state == JOB_RUNNING);
    CHECK(!memcmp(local_capture.pcm, a->pcm, sizeof(a->pcm)));
    CHECK(!memcmp(remote_capture.pcm, b->pcm, sizeof(b->pcm)));
    CHECK(remote_capture.generation == b->generation && active_generation == a->generation);
}
static void finish_job(void) {
    tick_step = 7; run_job(); tick_step = 0;
    CHECK(job_state == JOB_DONE && job_dsp_ms == 7);
    Ranging_Poll(); CHECK(job_state == JOB_IDLE);
}
static void expect_cmp(const char *window, const RangeEstimate *estimate, bool final) {
    char expected[256], q[12] = "NA", q2[12] = "NA", gap[12] = "NA", lag[12] = "NA", frac[12] = "NA";
    char dt[SYNC_I64_TEXT_SIZE] = "NA", mm[12] = "NA", elapsed[32] = "";
    if (estimate->correlation_valid) {
        snprintf(q, sizeof(q), "%u", estimate->quality_permille);
        snprintf(lag, sizeof(lag), "%d", estimate->peak_lag_samples);
        if (estimate->competitor_valid) {
            snprintf(q2, sizeof(q2), "%u", estimate->competitor_quality_permille);
            snprintf(gap, sizeof(gap), "%d", estimate->peak_margin_permille);
        }
    }
    if (estimate->candidate_valid) {
        snprintf(frac, sizeof(frac), "%d", (int)lroundf(estimate->peak_fraction_samples * 65536.0f));
        SyncFormat_I64(dt, estimate->candidate_raw_delay_ns);
        snprintf(mm, sizeof(mm), "%u", estimate->candidate_distance_mm);
    }
    if (final) snprintf(elapsed, sizeof(elapsed), " dsp_ms=%u", job_dsp_ms);
    snprintf(expected, sizeof(expected), "RANGE CMP a=%u b=%u win=%s error=%u q=%s q2=%s gap=%s lag=%s frac=%s dt_ns=%s mm=%s%s\r\n",
             local_capture.sequence, remote_capture.sequence, window, (unsigned)estimate->error,
             q, q2, gap, lag, frac, dt, mm, elapsed);
    CHECK(lines(expected) == 1);
}
static void wideband(RangeCapture *a, RangeCapture *b) {
    memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
    a->sequence = 101; b->sequence = 202; a->generation = 3; b->generation = 7;
    a->flags = b->flags = RANGE_CAPTURE_LOCKED;
    a->period_ps = b->period_ps = 20837209ULL;
    a->start_ns = 20000000000ULL; b->start_ns = a->start_ns - 2038000ULL;
    a->onset_ns = a->start_ns + RANGE_PRE_SAMPLES * a->period_ps / 1000ULL;
    b->onset_ns = b->start_ns + RANGE_PRE_SAMPLES * b->period_ps / 1000ULL;
    uint32_t random = 0x12345678U;
    for (unsigned i = 0; i < RANGE_SAMPLES; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        a->pcm[i] = b->pcm[i] = (int16_t)((int)(random % 8001U) - 4000);
    }
}
#if !HAVE_HISTORICAL_PCM
static void shared_front_independent_tail(RangeCapture *a, RangeCapture *b) {
    wideband(a, b);
    uint32_t random = 0x9e3779b9U;
    /* Every B position touched by the FRONT search, including linear
       interpolation's extra sample, must keep the shared wideband signal. */
    const unsigned last_front_sample = RANGE_FRONT_END - 1U + RANGE_SEARCH_SAMPLES + 1U;
    for (unsigned i = last_front_sample + 1U; i < RANGE_SAMPLES; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        b->pcm[i] = (int16_t)((int)(random % 8001U) - 4000);
    }
}
#endif
static void old_pcm_front_is_not_published(void) {
#if HAVE_HISTORICAL_PCM
    RangeCapture a = historical_a, b = historical_b;
#else
    RangeCapture a, b; shared_front_independent_tail(&a, &b);
#endif
    reset(true, a.onset_ns + 50000000ULL, a.generation);
    transfer_to_master(&a, &b); finish_job();
    RangingDiagnostics d; RangingStatus s;
    CHECK(Ranging_GetDiagnostics(&d) && Ranging_GetStatus(&s));
    CHECK(d.a_sequence == a.sequence && d.b_sequence == b.sequence && d.dsp_ms == 7);
    CHECK(d.long_window.error == RANGE_LOW_CORRELATION && d.long_window.candidate_valid);
    CHECK(d.front_window.error == RANGE_OK);
#if HAVE_HISTORICAL_PCM
    CHECK(d.long_window.quality_permille == 437 && d.long_window.peak_lag_samples == 12);
    CHECK(d.front_window.quality_permille == 962 && d.front_window.distance_mm == 699);
#else
    CHECK(d.long_window.quality_permille < 650);
    CHECK(d.front_window.quality_permille == 1000 && d.front_window.distance_mm == 700);
#endif
    CHECK(!s.valid && s.accepted == 0 && s.rejected == 1);
    CHECK(packets(RANGE_RESULT) == 0 && packets(RANGE_REJECT) == 1);
    CHECK(lines("RANGE CMP ") == 2 && lines("RANGE RESULT ") == 0 && lines("RANGE REJECT ") == 1);
    expect_cmp("LONG", &d.long_window, false); expect_cmp("FRONT", &d.front_window, true);
    advance(1800); Ranging_Poll(); CHECK(packets(RANGE_RESULT) == 0 && status.accepted == 0);
    /* A later accepted local capture must clear the published previous attempt. */
    a.sequence++; a.onset_ns = fake_now - 50000000ULL; a.start_ns = a.onset_ns - 5334325ULL;
    offer(&a); Ranging_Poll(); CHECK(!Ranging_GetDiagnostics(&d) && !d.valid);
    puts("PASS: LONG reject + FRONT pass remain diagnostics only; new capture clears attempt");
}
static void only_long_publishes_once(void) {
    RangeCapture a, b; wideband(&a, &b); reset(true, a.onset_ns + 50000000ULL, a.generation);
    transfer_to_master(&a, &b); finish_job();
    RangingDiagnostics d; CHECK(Ranging_GetDiagnostics(&d));
    CHECK(d.long_window.error == RANGE_OK && d.front_window.error == RANGE_OK);
    CHECK(stage == RESULT_TX && status.accepted == 0 && !status.valid);
    CHECK(packets(RANGE_RESULT) == 1 && next_result_id == 1);
    CHECK(result_packet.delay_ns == d.long_window.delay_ns && result_packet.distance_mm == d.long_window.distance_mm);
    CHECK(result_packet.a_sequence == a.sequence && result_packet.b_sequence == b.sequence);
    CHECK(result_packet.generation == b.generation && result_packet.apply_ns - fake_now == DISPLAY_LEAD_NS);
    CHECK(lines("RANGE CMP ") == 2); expect_cmp("LONG", &d.long_window, false); expect_cmp("FRONT", &d.front_window, true);
    RangePacket ack = {0}; ack.type = RANGE_RESULT_ACK; ack.sequence = result_packet.sequence; receive(&ack);
    CHECK(result_acked);
    advance(249); Ranging_Poll(); CHECK(status.accepted == 0 && lines("RANGE RESULT ") == 0);
    advance(1); Ranging_Poll(); CHECK(status.accepted == 1 && status.valid && status.distance_mm == 700);
    CHECK(lines("RANGE RESULT ") == 1 && stage == IDLE);
    for (unsigned i = 0; i < sent_count; ++i) if (sent[i].type == RANGE_RESULT) CHECK(sent[i].sequence == 1);
    advance(1000); Ranging_Poll(); CHECK(status.accepted == 1 && lines("RANGE RESULT ") == 1);
    /* Starting from peer metadata also clears the previous DSP attempt. */
    b.sequence++; b.onset_ns = fake_now - 50000000ULL; b.start_ns = b.onset_ns - 5334325ULL;
    metadata(&b); Ranging_Poll(); CHECK(!Ranging_GetDiagnostics(&d) && !d.valid);
    puts("PASS: one LONG result ID, peer ACK and 250 ms display; FRONT never adds accepted/result");
}
static void stale_dsp_is_discarded(bool epoch_change) {
    RangeCapture a, b; wideband(&a, &b); reset(true, a.onset_ns + 50000000ULL, a.generation);
    transfer_to_master(&a, &b);
    if (epoch_change) fake_epoch++;
    else Ranging_SetLink(true, false, local_session, remote_session);
    finish_job();
    RangingDiagnostics d;
    CHECK(!Ranging_GetDiagnostics(&d) && !d.valid && !diagnostics.valid);
    CHECK(status.accepted == 0 && !status.valid && packets(RANGE_RESULT) == 0);
    CHECK(lines("RANGE CMP ") == 0 && lines("RANGE RESULT ") == 0 && stage == IDLE);
    CHECK(!discard_job && job_state == JOB_IDLE);
    puts(epoch_change ? "PASS: epoch change discards completed DSP and snapshots" :
                        "PASS: sync loss discards completed DSP and snapshots");
}
static void slave_has_no_master_diagnostics(void) {
    RangeCapture a, b; wideband(&a, &b); reset(false, b.onset_ns + 50000000ULL, b.generation);
    offer(&b); Ranging_Poll(); CHECK(stage == META_TX && packets(RANGE_META) == 1);
    RangePacket ack = {0}; ack.type = RANGE_ACK; ack.sequence = b.sequence; receive(&ack);
    CHECK(stage == DATA_TX);
    for (unsigned offset = 0; offset < RANGE_SAMPLES; offset += RANGE_CHUNK_SAMPLES) {
        unsigned before = packets(RANGE_DATA); Ranging_Poll(); CHECK(packets(RANGE_DATA) == before+1);
        CHECK(sent[sent_count-1].offset == offset);
        CHECK(!memcmp(sent[sent_count-1].pcm, b.pcm+offset, sizeof(sent[0].pcm)));
        ack.offset = (uint16_t)(offset + RANGE_CHUNK_SAMPLES); receive(&ack);
    }
    CHECK(stage == RESULT_WAIT && packets(RANGE_DATA) == 128 && notified == 0);
    RangePacket result = {0}; result.type = RANGE_RESULT; result.sequence = 42;
    result.delay_ns = -2038000; result.distance_mm = 700; result.quality_permille = 1000;
    result.apply_ns = fake_now + DISPLAY_LEAD_NS; result.a_sequence = a.sequence;
    result.b_sequence = b.sequence; result.generation = b.generation;
    result.config_revision = temperature.revision; receive(&result);
    CHECK(stage == DISPLAY_WAIT && packets(RANGE_RESULT_ACK) == 1 && status.accepted == 0);
    advance(250); Ranging_Poll(); CHECK(status.accepted == 1 && status.valid && !status.master);
    RangingDiagnostics d; CHECK(!Ranging_GetDiagnostics(&d) && !d.valid && lines("RANGE CMP ") == 0);
    CHECK(packets(RANGE_RESULT) == 0 && notified == 0);
    /* A -> B role changes cannot retain an A-side comparison snapshot. */
    diagnostics.valid = true; publish(); CHECK(Ranging_GetDiagnostics(&d));
    Ranging_SetLink(true, true, 111, 222); Ranging_Poll();
    diagnostics.valid = true; publish();
    Ranging_SetLink(false, true, 111, 222); Ranging_Poll();
    CHECK(!Ranging_GetDiagnostics(&d) && !d.valid);
    puts("PASS: real slave META/DATA/ACK/RESULT path publishes no master DSP snapshot");
}
static void log_bounds(void) {
    RangeCapture a, b; wideband(&a, &b); reset(true, a.onset_ns + 50000000ULL, a.generation);
    local_capture.sequence = remote_capture.sequence = UINT32_MAX;
    RangeEstimate r = {0}; r.correlation_valid = r.competitor_valid = r.candidate_valid = true;
    r.quality_permille = r.competitor_quality_permille = UINT32_MAX;
    r.peak_margin_permille = r.peak_lag_samples = INT32_MIN;
    r.peak_fraction_samples = -.5f; r.candidate_raw_delay_ns = INT64_MIN;
    r.candidate_distance_mm = UINT32_MAX; job_dsp_ms = UINT32_MAX;
    log_comparison("LONG", &r, false); log_comparison("FRONT", &r, true);
    CHECK(strstr(logs[log_count-1], "frac=-32768 dt_ns=-9223372036854775808 mm=4294967295 dsp_ms=4294967295\r\n"));
    r = (RangeEstimate){0}; log_comparison("FRONT", &r, false);
    CHECK(strstr(logs[log_count-1], "q=NA q2=NA gap=NA lag=NA frac=NA dt_ns=NA mm=NA\r\n"));
    CHECK(lines("RANGE CMP ") == 3);
    puts("PASS: startup/CMP lines fit buffers, invalid fields are NA, Q16 integer formatting needs no float printf");
}
static void temperature_change_discards_old_dsp(void) {
    RangeCapture a, b; wideband(&a, &b); reset(true, a.onset_ns + 50000000ULL, a.generation);
    transfer_to_master(&a, &b);
    CHECK(job_config.sound_speed_mm_s == RANGE_SOUND_SPEED_MM_S && job_config_revision == 1);
    diagnostics.valid = true; publish();
    RangePacket change = {0}; change.type = RANGE_CONFIG_REQUEST;
    change.sequence = 1; change.temperature_c_x10 = 300; change.source_epoch = source_epoch;
    receive(&change);
    CHECK(temperature.revision == 2 && config.sound_speed_mm_s == Range_TemperatureSoundSpeed(300));
    CHECK(discard_job && !diagnostics.valid && !published_diagnostics.valid);
    finish_job();
    /* Both windows retain the same immutable pre-change sound speed. The
       canceled result and its diagnostics must never publish under rev 2. */
    CHECK(job_result.distance_mm == 700 && job_front_result.distance_mm == 700);
    CHECK(!diagnostics.valid && status.accepted == 0 && packets(RANGE_RESULT) == 0);
    CHECK(lines("RANGE CMP ") == 0 && job_state == JOB_IDLE && !discard_job);
    puts("PASS: temperature change preserves both DSP snapshots and discards old-revision diagnostics/results");
}
static void temperature_revalues_without_new_measurement(void) {
    RangeCapture a, b; wideband(&a, &b); reset(true, a.onset_ns + 50000000ULL, a.generation);
    transfer_to_master(&a, &b); finish_job();
    RangePacket ack = {0}; ack.type = RANGE_RESULT_ACK; ack.sequence = result_packet.sequence; receive(&ack);
    advance(250); Ranging_Poll();
    CHECK(status.accepted == 1 && status.valid && status.distance_mm == 700 && diagnostics.valid);
    CHECK(Ranging_SetTemperature(300)); Ranging_Poll();
    uint32_t speed = Range_TemperatureSoundSpeed(300);
    uint32_t expected = (uint32_t)(((uint64_t)(-accepted_delay_ns) * speed + 500000000ULL) / 1000000000ULL);
    CHECK(status.distance_mm == expected && status.distance_mm > 700);
    CHECK(status.sequence == 1 && status.accepted == 1 && lines("RANGE RESULT ") == 1);
    CHECK(!diagnostics.valid && !published_diagnostics.valid);
    ack = (RangePacket){0}; ack.type = RANGE_CONFIG_ACK; ack.sequence = temperature.revision;
    ack.temperature_c_x10 = 300; ack.source_epoch = source_epoch; receive(&ack); Ranging_Poll();
    RangingTemperatureStatus snapshot; CHECK(Ranging_GetTemperature(&snapshot));
    CHECK(snapshot.temperature_c_x10 == 300 && snapshot.sound_speed_mm_s == speed);
    CHECK(snapshot.synchronized && !snapshot.pending);
    puts("PASS: manual temperature synchronization revalues the cached distance without counting another sound");
}
int main(void) {
    old_pcm_front_is_not_published(); only_long_publishes_once();
    stale_dsp_is_discarded(false); stale_dsp_is_discarded(true);
    slave_has_no_master_diagnostics(); log_bounds();
    temperature_change_discards_old_dsp(); temperature_revalues_without_new_measurement();
    puts("All ranging flow integration checks passed."); return 0;
}
'''


def capture_initializer(path, name):
    metadata = json.loads(path.read_text(encoding="utf-8-sig"))
    raw = path.with_suffix(".pcm").read_bytes()
    assert len(raw) == 4096 and f"{zlib.crc32(raw):08x}" == metadata["crc32"]
    values = struct.unpack("<2048h", raw)
    fields = [f".sequence={int(metadata['sequence'])}U", f".generation={int(metadata['epoch'])}U",
              f".flags={int(metadata['flags'])}U"]
    fields.extend(f".{key}={int(metadata[key])}ULL" for key in ("onset_ns", "start_ns", "period_ps"))
    fields.append(".pcm={" + ",".join(map(str, values)) + "}")
    return f"static const RangeCapture {name} = {{" + ",".join(fields) + "};\n"


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--captures", type=Path,
                        default=root / "build/pcm-capture/20261023_214834_384")
    args = parser.parse_args()
    work = root / "build/ranging-flow-validation"
    work.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    stubs = run / "stubs"
    stubs.mkdir()
    (stubs / "main.h").write_text("#include <stdint.h>\nuint32_t HAL_GetTick(void);\n", encoding="ascii")
    (stubs / "FreeRTOS.h").write_text("""#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;
typedef struct { int unused; } StaticTask_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1U
#define tskIDLE_PRIORITY 0U
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
""", encoding="ascii")
    (stubs / "task.h").write_text("""#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, uint32_t, void *, unsigned, StackType_t *, StaticTask_t *);
uint32_t ulTaskNotifyTake(int, uint32_t);
void xTaskNotifyGive(TaskHandle_t);
""", encoding="ascii")
    captures = [(args.captures / "COM3_0001_A_seq1.json", "historical_a"),
                (args.captures / "COM8_0001_B_seq1.json", "historical_b")]
    have_history = all(p.is_file() and p.with_suffix(".pcm").is_file() for p, _ in captures)
    historical = f"#define HAVE_HISTORICAL_PCM {int(have_history)}\n"
    if have_history:
        historical += "".join(capture_initializer(p, name) for p, name in captures)
        print("Historical PCM replay:", args.captures)
    else:
        print("SKIP: historical PCM replay unavailable; using deterministic shared-front/independent-tail fixture.")
    (run / "historical.h").write_text(historical, encoding="ascii")
    fixture = run / "flow.c"
    fixture.write_text(HARNESS, encoding="ascii")
    exe = run / "flow.exe"
    cc = shutil.which(args.cc) or args.cc
    command = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I" + str(stubs), "-I" + str(root / "App"), str(fixture),
               str(root / "App/Ranging/range_core.c"), str(root / "App/Ranging/range_temperature.c"),
               str(root / "App/TimeSync/sync_format.c"),
               "-lm", "-o", str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (run / "native-output.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    if result.returncode:
        raise AssertionError(result.stderr + f"\nArtifacts: {run}")
    print("Artifacts:", run)


if __name__ == "__main__":
    main()
