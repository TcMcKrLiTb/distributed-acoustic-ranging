"""Validate real EXTI/TIM14 acquisition, pulse decoding and LibDriver together.

IRQ events advance independently of the simulated sleeping task. These are host
logic tests, not a physical latency measurement on the STM32.
"""

from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "App" / "DHT11"
DRIVER = ROOT / "Drivers" / "LibDriver" / "DHT11"
BUILD = ROOT / "build" / "dht11-validation" / "port"

MAIN_STUB = r'''
#ifndef TEST_MAIN_H
#define TEST_MAIN_H
#include <stdint.h>
typedef struct { uint32_t CR1, DIER, CCER, EGR, SR, CNT; } TIM_TypeDef;
typedef struct { uint32_t Prescaler, Period, CounterMode, ClockDivision,
                         AutoReloadPreload; } TIM_Base_InitTypeDef;
typedef struct { TIM_TypeDef *Instance; TIM_Base_InitTypeDef Init; } TIM_HandleTypeDef;
typedef struct { uint32_t CFGR, DCKCFGR1; } RCC_TypeDef;
typedef struct { uint32_t BSRR, IDR; } GPIO_TypeDef;
typedef struct { uint32_t IMR, EMR, RTSR, FTSR, PR; } EXTI_TypeDef;
typedef struct { uint32_t EXTICR[4]; } SYSCFG_TypeDef;
extern RCC_TypeDef mock_rcc;
extern GPIO_TypeDef mock_gpio;
extern EXTI_TypeDef mock_exti;
extern SYSCFG_TypeDef mock_syscfg;
TIM_TypeDef *mock_timer(void);
void mock_syscfg_enable(void);
#define TIM14 mock_timer()
#define RCC (&mock_rcc)
#define EXTI (&mock_exti)
#define SYSCFG (&mock_syscfg)
#define DHT11_SIG_GPIO_Port (&mock_gpio)
#define DHT11_SIG_Pin (1U << 6)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_9 (1U << 9)
#define TIM_CR1_CEN 1U
#define TIM_CR1_OPM 8U
#define TIM_EGR_UG 1U
#define RCC_CFGR_PPRE1_Pos 10U
#define RCC_CFGR_PPRE1 (7U << RCC_CFGR_PPRE1_Pos)
#define RCC_DCKCFGR1_TIMPRE (1U << 24)
#define TIM_COUNTERMODE_UP 0U
#define TIM_CLOCKDIVISION_DIV1 0U
#define TIM_AUTORELOAD_PRELOAD_DISABLE 0U
#define SYSCFG_EXTICR2_EXTI6 (15U << 8)
#define SYSCFG_EXTICR2_EXTI6_PG (6U << 8)
#define EXTI9_5_IRQn 23
#define HAL_OK 0U
#define __HAL_RCC_SYSCFG_CLK_ENABLE() mock_syscfg_enable()
#define MODIFY_REG(reg, clear, set) ((reg) = ((reg) & ~(clear)) | (set))
#define __DMB() ((void)0)
#define __DSB() ((void)0)
uint32_t HAL_RCC_GetPCLK1Freq(void);
uint32_t HAL_GetTick(void);
unsigned HAL_TIM_Base_Init(TIM_HandleTypeDef *timer);
unsigned HAL_TIM_Base_Start(TIM_HandleTypeDef *timer);
void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned subpriority);
void HAL_NVIC_ClearPendingIRQ(int irq);
void HAL_NVIC_EnableIRQ(int irq);
#endif
'''

RTOS_STUB = r'''
#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <assert.h>
#include <stdint.h>
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
#define configASSERT(condition) assert(condition)
#endif
'''
TASK_STUB = r'''
#ifndef TEST_TASK_H
#define TEST_TASK_H
void vTaskDelay(uint32_t ticks);
#endif
'''

HARNESS = r'''
#include "main.h"
#include "driver_dht11_interface.h"
#include "dht11_port.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

RCC_TypeDef mock_rcc;
GPIO_TypeDef mock_gpio;
EXTI_TypeDef mock_exti;
SYSCFG_TypeDef mock_syscfg;
static TIM_TypeDef registers;
TIM_HandleTypeDef htim14;
static uint32_t pclk_hz, increment, timer_reads;
static unsigned init_result, start_result, init_calls, start_calls;
static unsigned failures, assertions, irq_priority, irq_enables, syscfg_enabled;
static unsigned delayed, delivered, task_lateness, pulse_high_override, edge_latency;
static uint64_t time_us;
static uint16_t timer_offset;
static uint8_t frame[5];
enum Mode { NORMAL, ABSENT, STUCK_LOW, TRUNCATED };
static enum Mode mode;
static bool awaiting_release, frame_active, irq_enabled;
static uint64_t released_at;
static unsigned edge_index, edge_count;
static struct { unsigned offset; bool high; } events[85];
void EXTI9_5_IRQHandler(void);

#define CHECK(condition) do { assertions++; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void advance_to(uint64_t now) {
    time_us = now;
    registers.CNT = (uint16_t)(now + timer_offset);
}
TIM_TypeDef *mock_timer(void) {
    timer_reads++;
    if (increment) registers.CNT = (registers.CNT + increment) & 65535U;
    return &registers;
}
void mock_syscfg_enable(void) { syscfg_enabled++; }
uint32_t HAL_RCC_GetPCLK1Freq(void) { return pclk_hz; }
uint32_t HAL_GetTick(void) { return (uint32_t)(time_us / 1000U); }
unsigned HAL_TIM_Base_Init(TIM_HandleTypeDef *timer) {
    CHECK(timer == &htim14); init_calls++; return init_result;
}
unsigned HAL_TIM_Base_Start(TIM_HandleTypeDef *timer) {
    CHECK(timer == &htim14); start_calls++;
    if (!start_result) registers.CR1 |= TIM_CR1_CEN;
    return start_result;
}
void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned subpriority) {
    CHECK(irq == EXTI9_5_IRQn && subpriority == 0U); irq_priority = priority;
}
void HAL_NVIC_ClearPendingIRQ(int irq) { CHECK(irq == EXTI9_5_IRQn); }
void HAL_NVIC_EnableIRQ(int irq) {
    CHECK(irq == EXTI9_5_IRQn); irq_enabled = true; irq_enables++;
}
static void emit_edge(bool high) {
    mock_gpio.IDR = high ? DHT11_SIG_Pin : 0U;
    if (irq_enabled && (mock_exti.IMR & DHT11_SIG_Pin)) {
        mock_exti.PR |= DHT11_SIG_Pin;
        EXTI9_5_IRQHandler();
        mock_exti.PR = 0U; /* Emulate hardware write-one-to-clear PR. */
        delivered++;
    }
}
static void create_events(void) {
    edge_count = 0U;
    events[edge_count++] = (typeof(events[0])){0U, true};
    if (mode == ABSENT) return;
    events[edge_count++] = (typeof(events[0])){20U, false};
    if (mode == STUCK_LOW) return;
    events[edge_count++] = (typeof(events[0])){100U, true};
    events[edge_count++] = (typeof(events[0])){180U, false};
    unsigned now = 180U;
    for (unsigned bit = 0; bit < 40U; bit++) {
        now += 50U;
        events[edge_count++] = (typeof(events[0])){now + edge_latency, true};
        unsigned high = frame[bit / 8U] & (0x80U >> (bit % 8U)) ? 70U : 26U;
        if (bit == 0U && pulse_high_override) high = pulse_high_override;
        now += high;
        events[edge_count++] = (typeof(events[0])){now, false};
        if (mode == TRUNCATED && bit == 4U) return;
    }
    events[edge_count++] = (typeof(events[0])){now + 50U, true};
}
void vTaskDelay(uint32_t ticks) {
    delayed += ticks;
    if (mock_gpio.BSRR == (uint32_t)DHT11_SIG_Pin << 16U) {
        awaiting_release = true; frame_active = false;
        mock_gpio.IDR = 0U;
    } else if (awaiting_release) {
        awaiting_release = false; frame_active = true;
        released_at = time_us; edge_index = 0U; create_events();
    }
    unsigned late = frame_active ? task_lateness : 0U;
    uint64_t target = time_us + ticks * 1000U + late;
    while (frame_active && edge_index < edge_count &&
           released_at + events[edge_index].offset <= target) {
        advance_to(released_at + events[edge_index].offset);
        emit_edge(events[edge_index++].high);
    }
    advance_to(target);
}

static void reset(void) {
    memset(&registers, 0, sizeof(registers));
    memset(&mock_rcc, 0, sizeof(mock_rcc));
    memset(&mock_gpio, 0, sizeof(mock_gpio));
    memset(&mock_exti, 0, sizeof(mock_exti));
    memset(&mock_syscfg, 0, sizeof(mock_syscfg));
    memset(&htim14, 0, sizeof(htim14));
    htim14.Instance = &registers; pclk_hz = 54000000U;
    mock_rcc.CFGR = 5U << RCC_CFGR_PPRE1_Pos;
    mock_gpio.IDR = DHT11_SIG_Pin;
    init_result = start_result = init_calls = start_calls = 0U;
    increment = timer_reads = 0U;
    delayed = delivered = irq_priority = irq_enables = syscfg_enabled = 0U;
    time_us = timer_offset = task_lateness = pulse_high_override = edge_latency = 0U;
    awaiting_release = frame_active = irq_enabled = false;
    mode = NORMAL;
    frame[0] = 61U; frame[1] = 0U; frame[2] = 24U; frame[3] = 3U;
    frame[4] = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
}

static dht11_handle_t make_handle(void) {
    dht11_handle_t sensor = {
        .bus_init = dht11_interface_init, .bus_deinit = dht11_interface_deinit,
        .bus_read = dht11_interface_read, .bus_write = dht11_interface_write,
        .delay_ms = dht11_interface_delay_ms, .delay_us = dht11_interface_delay_us,
        .enable_irq = dht11_interface_enable_irq, .disable_irq = dht11_interface_disable_irq,
        .debug_print = dht11_interface_debug_print,
        .read_frame = dht11_interface_read_frame,
    };
    return sensor;
}
static void check_cleanup(void) {
    CHECK((mock_exti.IMR & DHT11_SIG_Pin) == 0U);
    CHECK(mock_gpio.BSRR == DHT11_SIG_Pin);
    CHECK(registers.CR1 & TIM_CR1_CEN);
    CHECK(registers.DIER == 0U && registers.CCER == 0U);
}
static void test_clock_and_gpio(void) {
    const struct { unsigned ppre, timpre, pclk, prescaler; } cases[] = {
        {0U, 0U, 108000000U, 107U}, {4U, 0U, 54000000U, 107U},
        {5U, 0U, 54000000U, 107U}, {5U, 1U, 54000000U, 215U},
        {6U, 1U, 27000000U, 107U}, {7U, 1U, 13500000U, 53U},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(); mock_rcc.CFGR = cases[i].ppre << RCC_CFGR_PPRE1_Pos;
        mock_rcc.DCKCFGR1 = cases[i].timpre ? RCC_DCKCFGR1_TIMPRE : 0U;
        pclk_hz = cases[i].pclk; registers.DIER = registers.CCER = registers.SR = 65535U;
        registers.CR1 = TIM_CR1_OPM;
        CHECK(DHT11_PortInit());
        CHECK(htim14.Init.Prescaler == cases[i].prescaler && htim14.Init.Period == 65535U);
        CHECK(registers.EGR == TIM_EGR_UG && registers.SR == 0U);
        CHECK(irq_priority == 3U && irq_enables == 1U && syscfg_enabled == 1U);
        CHECK(mock_syscfg.EXTICR[1] == SYSCFG_EXTICR2_EXTI6_PG);
        CHECK((mock_exti.RTSR & DHT11_SIG_Pin) && (mock_exti.FTSR & DHT11_SIG_Pin));
        check_cleanup();
    }
    reset(); htim14.Instance = NULL; CHECK(!DHT11_PortInit());
    reset(); registers.CR1 = TIM_CR1_CEN; CHECK(!DHT11_PortInit());
    reset(); pclk_hz = 400000U; CHECK(!DHT11_PortInit());
    reset(); pclk_hz = 54321000U; CHECK(!DHT11_PortInit());
    reset(); init_result = 1U; CHECK(!DHT11_PortInit()); CHECK(start_calls == 0U);
    reset(); start_result = 1U; CHECK(!DHT11_PortInit());
    CHECK(dht11_interface_init() != 0U);
    for (unsigned bit = 5U; bit <= 9U; bit++) {
        if (bit == 6U) continue;
        reset(); mock_exti.IMR = 1U << bit; CHECK(!DHT11_PortInit());
    }
    reset(); CHECK(DHT11_PortInit());
    uint8_t value = 99U;
    CHECK(dht11_interface_read(&value) == 0U && value == 1U);
    mock_gpio.IDR = 0U; CHECK(dht11_interface_read(&value) == 0U && value == 0U);
    CHECK(dht11_interface_read(NULL) != 0U);
    CHECK(dht11_interface_write(0U) == 0U && mock_gpio.BSRR == (uint32_t)DHT11_SIG_Pin << 16U);
    CHECK(dht11_interface_write(1U) == 0U && mock_gpio.BSRR == DHT11_SIG_Pin);
    CHECK(dht11_interface_read_frame(NULL) != 0U);
    DHT11_PortBegin(); uint32_t elapsed;
    CHECK(DHT11_PortFinish(&elapsed) && elapsed == 0U);
    CHECK(DHT11_PortFinish(NULL));
    registers.CNT = 65530U; increment = 1U;
    dht11_interface_delay_us(40U);
    CHECK(registers.CNT < 100U);
    CHECK(dht11_interface_deinit() == 0U);
    check_cleanup();
}

static void test_real_frame_path(void) {
    reset(); CHECK(DHT11_PortInit()); dht11_handle_t sensor = make_handle();
    uint32_t response;
    DHT11_PortBegin(); CHECK(dht11_init(&sensor) == 0U);
    CHECK(DHT11_PortFinish(&response) && DHT11_PortEdges() == 84U);
    CHECK(sensor.inited == 1U && response > 3000U && response < 6000U);
    check_cleanup();
    /* A task can resume 20 ms late while each EXTI edge was still timestamped. */
    task_lateness = 20000U; timer_offset = 65000U; edge_latency = 4U;
    uint16_t raw_t = 0U, raw_h = 0U; float temperature = 0.0f; uint8_t humidity = 0U;
    for (unsigned i = 0U; i < 64U; i++) {
        DHT11_PortBegin();
        CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) == 0U);
        CHECK(DHT11_PortFinish(&response));
        CHECK(temperature > 24.29f && temperature < 24.31f && humidity == 61U);
        CHECK(raw_t == ((24U << 8U) | 3U) && raw_h == (61U << 8U));
        CHECK(DHT11_PortEdges() == 84U && response > 3000U && response < 6000U);
        check_cleanup();
    }
    task_lateness = 0U; edge_latency = 0U;
    frame[0] = 100U; frame[2] = 20U; frame[3] = 137U;
    frame[4] = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) == 0U);
    CHECK(DHT11_PortFinish(&response));
    CHECK(temperature < -20.89f && temperature > -20.91f && humidity == 100U);
    DHT11_PortBegin(); CHECK(dht11_read_temperature(&sensor, &raw_t, &temperature) == 0U);
    CHECK(DHT11_PortFinish(&response));
    DHT11_PortBegin(); CHECK(dht11_read_humidity(&sensor, &raw_h, &humidity) == 0U);
    CHECK(DHT11_PortFinish(&response));

    frame[4] ^= 1U;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) != 0U);
    CHECK(DHT11_PortFinish(&response)); /* Complete waveform, bad checksum. */
    CHECK(strstr(DHT11_PortDiagnostic(), "check failed") != NULL);
    check_cleanup();
    frame[4] ^= 1U;
    mode = ABSENT;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) != 0U);
    CHECK(DHT11_PortFinish(&response) && DHT11_PortEdges() == 1U);
    check_cleanup();
    mode = STUCK_LOW;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) != 0U);
    CHECK(!DHT11_PortFinish(&response) && DHT11_PortEdges() == 2U);
    check_cleanup();
    mode = TRUNCATED;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) != 0U);
    CHECK(!DHT11_PortFinish(&response));
    check_cleanup();
    mode = NORMAL; pulse_high_override = 50U;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) != 0U);
    CHECK(!DHT11_PortFinish(&response));
    CHECK(strstr(DHT11_PortDiagnostic(), "capture error") != NULL);
    check_cleanup();
    pulse_high_override = 0U;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_t, &temperature, &raw_h, &humidity) == 0U);
    CHECK(DHT11_PortFinish(&response));
    check_cleanup();
}
int main(void) {
    test_clock_and_gpio(); test_real_frame_path();
    printf("DHT11 EXTI port: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
'''


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    for name, text in {"main.h": MAIN_STUB, "FreeRTOS.h": RTOS_STUB,
                       "task.h": TASK_STUB, "port_harness.c": HARNESS}.items():
        (BUILD / name).write_text(text, encoding="utf-8")
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler:
        raise SystemExit("Native GCC is required (or set CC).")
    executable = BUILD / ("test_dht11_port.exe" if os.name == "nt" else "test_dht11_port")
    source = PORT / "driver_dht11_interface.c"
    code = source.read_text(encoding="utf-8")
    forbidden = ("__disable_irq(", "__enable_irq(", "__set_PRIMASK(",
                 "taskENTER_CRITICAL(", "vTaskSuspendAll(", "xTaskResumeAll(")
    if any(token in code for token in forbidden):
        raise AssertionError("Port must preserve IRQs and task scheduling.")
    subprocess.run([
        compiler, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(BUILD), "-I", str(DRIVER), "-I", str(PORT),
        str(source), str(PORT / "dht11_capture.c"), str(DRIVER / "driver_dht11.c"),
        str(BUILD / "port_harness.c"), "-o", str(executable),
    ], check=True, cwd=ROOT)
    subprocess.run([str(executable)], check=True, cwd=ROOT, timeout=10)


if __name__ == "__main__":
    main()
