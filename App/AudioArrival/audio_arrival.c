#include "audio_arrival.h"
#include "main.h"
#include "arrival_detector.h"
#include "arrival_time.h"
#include "arrival_clock.h"
#include "Ranging/range_recorder.h"
#include "Diagnostics/pcm_dump.h"
#include "TimeSync/time_sync.h"
#include "TimeSync/Port/sync_log.h"
#include "stm32746g_discovery_audio.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Use the same physical channel on both boards. Never average the two mics. */
#define MIC_CHANNEL 0U
#define INPUT_VOLUME 86U
#define QUEUE_DEPTH 6U
#define INPUT_DELAY_NS 0ULL /* Measured codec/acoustic delay can be calibrated later. */

extern SAI_HandleTypeDef haudio_in_sai, haudio_out_sai;
static int16_t dma_pcm[ARRIVAL_DMA_WORDS]
    __attribute__((section(".audio_dma"), aligned(32)));

typedef struct {
    int16_t pcm[ARRIVAL_BLOCK_FRAMES];
    uint64_t first_sample, last_ns;
    uint32_t generation;
    bool continuous;
} AudioBlock;

static StaticQueue_t queue_control;
static uint8_t queue_storage[QUEUE_DEPTH * sizeof(AudioBlock)];
static QueueHandle_t blocks;
static AudioBlock irq_block, task_block;
static ArrivalDetector detector;
static AudioArrivalEvent latest;
static bool latest_valid, hardware_ok;
static uint64_t sample_period_ps, next_sample, previous_anchor;
static uint32_t previous_generation;
static volatile uint32_t irq_blocks, dropped_blocks, hardware_errors, timing_errors;
static volatile uint32_t clock_skips;
static volatile bool stream_fault;

static RangeRecorder recorder;
static ArrivalClock sample_clock;
static uint64_t mapped_period_ps;
static RangeCapture captures[2];
/* AudioTask writes outside the critical section; consumer claims before copying. */
enum { CAP_FREE, CAP_WRITING, CAP_READY, CAP_READING };
static uint8_t capture_states[2];

static void reset_recorder(void) {
    RangeRecorder_Reset(&recorder);
    ArrivalClock_Reset(&sample_clock);
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < 2; ++i)
        if (capture_states[i] == CAP_READY) capture_states[i] = CAP_FREE;
    taskEXIT_CRITICAL();
}
static void finish_capture(void) {
    if (!RangeRecorder_Ready(&recorder)) return;
    int slot = -1;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < 2; ++i)
        if (capture_states[i] == CAP_FREE) { slot = (int)i; capture_states[i] = CAP_WRITING; break; }
    taskEXIT_CRITICAL();
    if (slot < 0) { recorder.pending = false; return; }
    bool copied = RangeRecorder_Copy(&recorder, &captures[slot]);
    if (copied) PcmDump_Submit(&captures[slot]);
    taskENTER_CRITICAL();
    capture_states[slot] = copied ? CAP_READY : CAP_FREE;
    taskEXIT_CRITICAL();
}

static void receive_half(unsigned half) {
    uint64_t now = 0;
    uint32_t generation = 0, remaining = 0;
    bool captured = false;
    /* Capture progress and PTP together. DMA keeps running during critical
       sections, so retry if it advances while the clock registers are read. */
    for (unsigned retry = 0; retry < 4; retry++) {
        uint32_t before = __HAL_DMA_GET_COUNTER(haudio_in_sai.hdmarx);
        bool valid = TimeSync_CaptureClockFromISR(&now, &generation);
        remaining = __HAL_DMA_GET_COUNTER(haudio_in_sai.hdmarx);
        if (before == remaining && valid) { captured = true; break; }
    }
    uint64_t first = next_sample;
    next_sample += ARRIVAL_BLOCK_FRAMES;
    irq_blocks++;
    if (!captured) {
        clock_skips++;
        return;
    }
    if (!ArrivalTime_Boundary(now, remaining, half, sample_period_ps, &irq_block.last_ns)) {
        timing_errors++;
        return;
    }
    irq_block.first_sample = first;
    irq_block.generation = generation;
    uint64_t duration = ARRIVAL_BLOCK_FRAMES * sample_period_ps / 1000U;
    uint64_t tolerance = 4U * sample_period_ps / 1000U;
    irq_block.continuous = previous_anchor && generation == previous_generation &&
        irq_block.last_ns > previous_anchor &&
        irq_block.last_ns - previous_anchor + tolerance >= duration &&
        irq_block.last_ns - previous_anchor <= duration + tolerance;
    previous_anchor = irq_block.last_ns;
    previous_generation = generation;
    const int16_t *source = dma_pcm + half * (ARRIVAL_DMA_WORDS / 2U);
    for (unsigned i = 0; i < ARRIVAL_BLOCK_FRAMES; i++)
        irq_block.pcm[i] = source[i * 2U + MIC_CHANNEL];
    uint64_t unused;
    if (!ArrivalTime_Boundary(now, __HAL_DMA_GET_COUNTER(haudio_in_sai.hdmarx),
                              half, sample_period_ps, &unused)) {
        dropped_blocks++;
        return;
    }
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(blocks, &irq_block, &wake) != pdPASS) dropped_blocks++;
    portYIELD_FROM_ISR(wake);
}

void BSP_AUDIO_IN_HalfTransfer_CallBack(void) { receive_half(0); }
void BSP_AUDIO_IN_TransferComplete_CallBack(void) { receive_half(1); }
void BSP_AUDIO_IN_Error_CallBack(void) { hardware_errors++; stream_fault = true; }

static bool start_stream(void) {
    HAL_NVIC_DisableIRQ(DMA2_Stream7_IRQn);
    HAL_NVIC_DisableIRQ(SAI2_IRQn);
    if (HAL_SAI_DMAStop(&haudio_in_sai) != HAL_OK) return false;
    __HAL_SAI_DISABLE(&haudio_in_sai);
    __HAL_SAI_CLEAR_FLAG(&haudio_in_sai, SAI_FLAG_OVRUDR | SAI_FLAG_AFSDET |
                         SAI_FLAG_LFSDET | SAI_FLAG_WCKCFG);
    SET_BIT(haudio_in_sai.Instance->CR2, SAI_xCR2_FFLUSH);
    memset(dma_pcm, 0, sizeof(dma_pcm)); /* NOLOAD section is not cleared at reset. */
    xQueueReset(blocks);
    previous_anchor = 0;
    next_sample = 0;
    stream_fault = false;
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream7_IRQn);
    HAL_NVIC_ClearPendingIRQ(SAI2_IRQn);
    HAL_StatusTypeDef result = HAL_SAI_Receive_DMA(&haudio_in_sai,
                            (uint8_t *)dma_pcm, ARRIVAL_DMA_WORDS);
    HAL_NVIC_EnableIRQ(SAI2_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);
    return result == HAL_OK;
}

static void emit_event(const ArrivalDetection *onset, const AudioBlock *block,
                       bool clipped) {
    uint64_t now, timestamp;
    uint32_t generation;
    if (!TimeSync_CaptureClockFromISR(&now, &generation) ||
        generation != block->generation ||
        !ArrivalTime_At(block->last_ns, (int64_t)onset->sample - (int64_t)block->first_sample,
                        onset->fraction_q16, mapped_period_ps, &timestamp) ||
        timestamp < INPUT_DELAY_NS) return;
    TimeSyncStatus sync;
    bool synchronized = TimeSync_GetStatus(&sync) && sync.locked;
    AudioArrivalEvent event = {0};
    event.sequence = latest.sequence + 1U;
    event.clock_generation = generation;
    event.time_ns = timestamp - INPUT_DELAY_NS;
    event.interval_valid = ArrivalTime_Interval(latest_valid, latest.time_ns,
        latest.clock_generation, event.time_ns, generation, &event.interval_ns);
    event.sample = onset->sample;
    event.peak = onset->peak; event.noise = onset->noise; event.threshold = onset->threshold;
    event.confirmation_samples = onset->confirmation_samples;
    event.synchronized = synchronized; event.clipped = clipped || onset->peak >= 32760U;
    taskENTER_CRITICAL();
    latest = event; latest_valid = true;
    taskEXIT_CRITICAL();
    if (event.synchronized)
        RangeRecorder_Arm(&recorder, event.sequence, generation, onset->sample,
                          onset->fraction_q16, event.time_ns, mapped_period_ps,
                          RANGE_CAPTURE_LOCKED | (event.clipped ? RANGE_CAPTURE_CLIPPED : 0U));
    char interval_text[32] = "NA";
    if (event.interval_valid) {
        snprintf(interval_text, sizeof(interval_text), "%" PRIu32 ".%09" PRIu32,
                 (uint32_t)(event.interval_ns / 1000000000ULL),
                 (uint32_t)(event.interval_ns % 1000000000ULL));
    }
    char confirmation_text[32] = "";
    if (event.confirmation_samples > ARRIVAL_CONFIRM_MIN_SAMPLES)
        snprintf(confirmation_text, sizeof(confirmation_text), " confirm=%" PRIu32,
                 event.confirmation_samples);
    char line[288];
#if APP_LOG_VERBOSE
    snprintf(line, sizeof(line), "AUDIO EVENT seq=%" PRIu32 " arrival=%" PRIu32 ".%09" PRIu32
             " epoch=%" PRIu32 " sample=%" PRIu32 " peak=%" PRIu32
             " noise=%" PRIu32 " threshold=%" PRIu32 " sync=%s clipped=%u interval=%s%s\r\n",
             event.sequence, (uint32_t)(event.time_ns / 1000000000ULL),
             (uint32_t)(event.time_ns % 1000000000ULL), generation, (uint32_t)onset->sample,
             event.peak, event.noise, event.threshold, synchronized ? "LOCKED" : "LOCAL",
             event.clipped ? 1U : 0U, interval_text, confirmation_text);
#else
    snprintf(line, sizeof(line), "AUDIO EVENT seq=%" PRIu32 " arrival=%" PRIu32 ".%09" PRIu32
             " interval=%s peak=%" PRIu32 " noise=%" PRIu32 " threshold=%" PRIu32 " sync=%s%s%s\r\n",
             event.sequence, (uint32_t)(event.time_ns / 1000000000ULL),
             (uint32_t)(event.time_ns % 1000000000ULL), interval_text, event.peak,
             event.noise, event.threshold,
             synchronized ? "LOCKED" : "LOCAL", event.clipped ? " clipped=1" : "", confirmation_text);
#endif
    SyncLog_Write(line);
}

void audio_task(void const *argument) {
    (void)argument;
    if (!hardware_ok) {
        SyncLog_Write("AUDIO ERROR: codec/SAI initialization failed; check power and WM8994\r\n");
        vTaskSuspend(NULL);
    }
    uint64_t now;
    uint32_t generation;
    while (!TimeSync_CaptureClockFromISR(&now, &generation)) vTaskDelay(pdMS_TO_TICKS(10));
#if APP_LOG_VERBOSE
    SyncLog_Write("AUDIO INIT: onboard DMIC2 channel=0, 16bit stereo DMA, nominal_fs=48000, warmup=1s; interval=seconds.nanoseconds (first: NA)\r\n");
#else
    char init_line[112];
    snprintf(init_line, sizeof(init_line), "AUDIO INIT fs=48000 ch=%u gain=%u warmup=1s confirm=%u..%u\r\n",
             (unsigned)MIC_CHANNEL, (unsigned)INPUT_VOLUME,
             (unsigned)ARRIVAL_CONFIRM_MIN_SAMPLES, (unsigned)ARRIVAL_CONFIRM_MAX_SAMPLES);
    SyncLog_Write(init_line);
#endif
    if (!start_stream()) {
        SyncLog_Write("AUDIO ERROR: cannot start SAI RX DMA\r\n");
        vTaskSuspend(NULL);
    }
    ArrivalDetector_Reset(&detector);
    uint64_t expected_sample = 0;
    uint32_t active_generation = UINT32_MAX, last_report = 0, peak = 0, clip_count = 0;
#if APP_LOG_VERBOSE
    uint32_t seen_blocks = 0;
#else
    uint32_t seen_drops = 0, seen_timing = 0, seen_hwerr = 0, seen_skips = 0;
    bool ready_reported = false;
#endif
    TickType_t last_receive = xTaskGetTickCount();
    for (;;) {
        bool received = xQueueReceive(blocks, &task_block, pdMS_TO_TICKS(20)) == pdPASS;
        if (received) {
            last_receive = xTaskGetTickCount();
            if (!TimeSync_CaptureClockFromISR(&now, &generation) ||
                generation != task_block.generation) {
                ArrivalDetector_Reset(&detector);
                reset_recorder();
                continue;
            }
            if (!task_block.continuous || active_generation != task_block.generation ||
                expected_sample != task_block.first_sample) {
                ArrivalDetector_Reset(&detector);
                reset_recorder();
            }
            active_generation = task_block.generation;
            expected_sample = task_block.first_sample + ARRIVAL_BLOCK_FRAMES;
            ArrivalClock_Push(&sample_clock, task_block.last_ns, sample_period_ps,
                              &task_block.last_ns, &mapped_period_ps);
            bool clipped = false;
            for (unsigned i = 0; i < ARRIVAL_BLOCK_FRAMES; i++) {
                int32_t value = task_block.pcm[i];
                uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
                if (magnitude > peak) peak = magnitude;
                if (magnitude >= 32760U) { clipped = true; clip_count++; }
                ArrivalDetection onset;
                RangeRecorder_Push(&recorder, task_block.first_sample + i, (int16_t)value);
                if (ArrivalDetector_Push(&detector, (int16_t)value,
                                          task_block.first_sample + i, &onset))
                    emit_event(&onset, &task_block, clipped);
            }
            finish_capture();
        }
        if (stream_fault || xTaskGetTickCount() - last_receive > pdMS_TO_TICKS(100)) {
            SyncLog_Write("AUDIO RECOVER: RX stalled/error; discard onset and restart DMA\r\n");
            if (!start_stream()) {
                SyncLog_Write("AUDIO ERROR: DMA restart failed; reset board\r\n");
                vTaskSuspend(NULL);
            }
            ArrivalDetector_Reset(&detector);
            reset_recorder();
            active_generation = UINT32_MAX;
            last_receive = xTaskGetTickCount();
        }
#if !APP_LOG_VERBOSE
        // HOLDOFF/ARMED cycles are expected for each event; do not log them.
        if (detector.warmup) ready_reported = false;
        else if (!ready_reported) {
            SyncLog_Write("AUDIO READY\r\n");
            ready_reported = true;
        }
#endif
        uint32_t ms = HAL_GetTick();
        if (ms - last_report >= 1000U) {
            last_report = ms;
#if APP_LOG_VERBOSE
            char line[256];
            snprintf(line, sizeof(line), "AUDIO STATUS state=%s blocks=%" PRIu32
                " rate=%" PRIu32 " peak=%" PRIu32 " noise=%" PRIu32
                " clip=%" PRIu32 " drop=%" PRIu32 " timing=%" PRIu32 " hwerr=%" PRIu32
                " clock_skip=%" PRIu32 " period_ps=%" PRIu32 "\r\n",
                detector.warmup ? "WARMUP" : detector.blocked ? "HOLDOFF" : "ARMED",
                irq_blocks, irq_blocks - seen_blocks, peak, detector.noise_q8 / 256U,
                clip_count, dropped_blocks, timing_errors, hardware_errors, clock_skips,
                (uint32_t)sample_period_ps);
            SyncLog_Write(line);
            seen_blocks = irq_blocks;
#else
            uint32_t drops = dropped_blocks, timing = timing_errors;
            uint32_t hwerr = hardware_errors, skips = clock_skips;
            if (drops != seen_drops || timing != seen_timing ||
                hwerr != seen_hwerr || skips != seen_skips || clip_count) {
                char line[128];
                snprintf(line, sizeof(line), "AUDIO WARN drop=%" PRIu32 " timing=%" PRIu32
                         " hwerr=%" PRIu32 " clip=%" PRIu32 " skip=%" PRIu32 "\r\n",
                         drops, timing, hwerr, clip_count, skips);
                SyncLog_Write(line);
            }
            seen_drops = drops; seen_timing = timing;
            seen_hwerr = hwerr; seen_skips = skips;
#endif
            peak = 0; clip_count = 0;
        }
    }
}

void AudioArrival_Init(void) {
    blocks = xQueueCreateStatic(QUEUE_DEPTH, sizeof(AudioBlock), queue_storage, &queue_control);
    hardware_ok = BSP_AUDIO_IN_Init(AUDIO_FREQUENCY_48K, 16, 2) == AUDIO_OK;
    if (hardware_ok) {
        hardware_ok = BSP_AUDIO_IN_SetVolume(INPUT_VOLUME) == AUDIO_OK &&
            HAL_SAI_GetState(&haudio_in_sai) == HAL_SAI_STATE_READY;
        uint32_t clock = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI2);
        uint32_t divider = haudio_out_sai.Init.Mckdiv;
        sample_period_ps = clock ? (1000000000000ULL * (divider ? 512U * divider : 256U)) / clock : 0;
        if (!sample_period_ps) hardware_ok = false;
        /* BSP enables both blocks to feed the codec's MCLK. Leave A running,
           but flush B before starting DMA after the scheduler starts. */
        __HAL_SAI_DISABLE(&haudio_in_sai);
        HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 5, 0);
        HAL_NVIC_SetPriority(SAI2_IRQn, 5, 0);
    }
}

bool AudioArrival_GetLatest(AudioArrivalEvent *event) {
    if (!event) return false;
    taskENTER_CRITICAL();
    *event = latest;
    bool valid = latest_valid;
    taskEXIT_CRITICAL();
    return valid;
}

bool AudioArrival_TakeCapture(RangeCapture *capture) {
    if (!capture) return false;
    int slot = -1;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < 2; ++i) {
        if (capture_states[i] == CAP_READY &&
            (slot < 0 || (int32_t)(captures[i].sequence - captures[slot].sequence) > 0)) slot = (int)i;
    }
    if (slot >= 0) {
        capture_states[slot] = CAP_READING;
        for (unsigned i = 0; i < 2; ++i)
            if ((int)i != slot && capture_states[i] == CAP_READY) capture_states[i] = CAP_FREE;
    }
    taskEXIT_CRITICAL();
    if (slot < 0) return false;
    *capture = captures[slot];
    taskENTER_CRITICAL(); capture_states[slot] = CAP_FREE; taskEXIT_CRITICAL();
    return true;
}
