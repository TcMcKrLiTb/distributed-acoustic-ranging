"""Validate the real STM32 DHT11 port against register/RTOS stubs.

The fake TIM14 can advance on each access and inject interrupt latency. This is
logic validation, not a measurement of compiled MCU or physical bus timing.
"""

from pathlib import Path
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "dht11-validation" / "port"
PORT = ROOT / "App" / "DHT11"
DRIVER = ROOT / "Drivers" / "LibDriver" / "DHT11"

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
extern RCC_TypeDef mock_rcc;
extern GPIO_TypeDef mock_gpio;
TIM_TypeDef *mock_timer(void);
#define TIM14 mock_timer()
#define RCC (&mock_rcc)
#define DHT11_SIG_GPIO_Port (&mock_gpio)
#define DHT11_SIG_Pin (1U << 6)
#define TIM_CR1_CEN 1U
#define TIM_CR1_OPM 8U
#define TIM_EGR_UG 1U
#define RCC_CFGR_PPRE1_Pos 10U
#define RCC_CFGR_PPRE1 (7U << RCC_CFGR_PPRE1_Pos)
#define RCC_DCKCFGR1_TIMPRE (1U << 24)
#define TIM_COUNTERMODE_UP 0U
#define TIM_CLOCKDIVISION_DIV1 0U
#define TIM_AUTORELOAD_PRELOAD_DISABLE 0U
#define HAL_OK 0U
uint32_t HAL_RCC_GetPCLK1Freq(void);
unsigned HAL_TIM_Base_Init(TIM_HandleTypeDef *timer);
unsigned HAL_TIM_Base_Start(TIM_HandleTypeDef *timer);
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
void vTaskSuspendAll(void);
int xTaskResumeAll(void);
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
static TIM_TypeDef registers;
TIM_HandleTypeDef htim14;
static uint32_t pclk_hz, increment, timer_reads, spike_read, spike_us;
static unsigned init_result, start_result, init_calls, start_calls;
static unsigned suspends, resumes, delayed, failures, assertions;
static bool waveform_enabled, awaiting_release, frame_active, latency_pending;
static uint16_t released_at;
static uint8_t frame[5];

#define CHECK(condition) do { assertions++; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static bool sensor_level(uint16_t elapsed) {
    if (elapsed < 20U) return true;
    if (elapsed < 100U) return false;
    if (elapsed < 180U) return true;
    elapsed -= 180U;
    for (unsigned bit = 0U; bit < 40U; bit++) {
        if (elapsed < 50U) return false;
        elapsed -= 50U;
        unsigned high = frame[bit / 8U] & (0x80U >> (bit % 8U)) ? 70U : 26U;
        if (elapsed < high) return true;
        elapsed -= high;
    }
    return elapsed >= 50U;
}

TIM_TypeDef *mock_timer(void) {
    timer_reads++;
    registers.CNT = (registers.CNT + increment +
                     (timer_reads == spike_read ? spike_us : 0U)) & 65535U;
    if (waveform_enabled) {
        if (mock_gpio.BSRR == (uint32_t)DHT11_SIG_Pin << 16U) {
            awaiting_release = true;
            frame_active = false;
            mock_gpio.IDR = 0U;
        } else if (awaiting_release) {
            released_at = (uint16_t)registers.CNT;
            awaiting_release = false;
            frame_active = true;
        }
        if (frame_active) {
            uint16_t elapsed = (uint16_t)(registers.CNT - released_at);
            if (latency_pending && elapsed >= 500U) {
                registers.CNT = (registers.CNT + 50U) & 65535U;
                elapsed += 50U;
                latency_pending = false;
            }
            mock_gpio.IDR = sensor_level(elapsed) ? DHT11_SIG_Pin : 0U;
        }
    }
    return &registers;
}
uint32_t HAL_RCC_GetPCLK1Freq(void) { return pclk_hz; }
unsigned HAL_TIM_Base_Init(TIM_HandleTypeDef *timer) {
    CHECK(timer == &htim14);
    init_calls++;
    return init_result;
}
unsigned HAL_TIM_Base_Start(TIM_HandleTypeDef *timer) {
    CHECK(timer == &htim14);
    start_calls++;
    if (!start_result) registers.CR1 |= TIM_CR1_CEN;
    return start_result;
}
void vTaskDelay(uint32_t ticks) {
    delayed += ticks;
    if (waveform_enabled) registers.CNT = (registers.CNT + ticks * 1000U) & 65535U;
}
void vTaskSuspendAll(void) { suspends++; }
int xTaskResumeAll(void) { resumes++; return 0; }

static void reset(void) {
    memset(&registers, 0, sizeof(registers));
    memset(&mock_rcc, 0, sizeof(mock_rcc));
    memset(&mock_gpio, 0, sizeof(mock_gpio));
    memset(&htim14, 0, sizeof(htim14));
    htim14.Instance = &registers;
    pclk_hz = 54000000U;
    mock_rcc.CFGR = 5U << RCC_CFGR_PPRE1_Pos;
    init_result = start_result = init_calls = start_calls = 0U;
    increment = timer_reads = spike_read = spike_us = 0U;
    suspends = resumes = delayed = 0U;
    waveform_enabled = awaiting_release = frame_active = latency_pending = false;
}

static void test_clock(void) {
    const struct { unsigned ppre, timpre, pclk, prescaler; } cases[] = {
        {0U, 0U, 108000000U, 107U},
        {4U, 0U, 54000000U, 107U},
        {5U, 0U, 54000000U, 107U},
        {5U, 1U, 54000000U, 215U},
        {6U, 1U, 27000000U, 107U},
        {7U, 1U, 13500000U, 53U},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        mock_rcc.CFGR = cases[i].ppre << RCC_CFGR_PPRE1_Pos;
        mock_rcc.DCKCFGR1 = cases[i].timpre ? RCC_DCKCFGR1_TIMPRE : 0U;
        pclk_hz = cases[i].pclk;
        registers.DIER = registers.CCER = registers.SR = 0xFFFFU;
        registers.CR1 = TIM_CR1_OPM;
        CHECK(DHT11_PortInit());
        CHECK(htim14.Init.Prescaler == cases[i].prescaler);
        CHECK(htim14.Init.Period == 65535U);
        CHECK(registers.DIER == 0U && registers.CCER == 0U && registers.SR == 0U);
        CHECK(registers.CR1 == TIM_CR1_CEN);
        CHECK(registers.EGR == TIM_EGR_UG && registers.CNT == 0U);
        CHECK(mock_gpio.BSRR == DHT11_SIG_Pin);
        CHECK(init_calls == 1U && start_calls == 1U);
    }
    reset(); htim14.Instance = NULL; CHECK(!DHT11_PortInit());
    reset(); registers.CR1 = TIM_CR1_CEN; CHECK(!DHT11_PortInit());
    reset(); pclk_hz = 400000U; CHECK(!DHT11_PortInit());
    reset(); pclk_hz = 54321000U; CHECK(!DHT11_PortInit());
    reset(); init_result = 1U; CHECK(!DHT11_PortInit()); CHECK(start_calls == 0U);
    reset(); start_result = 1U; CHECK(!DHT11_PortInit());
    CHECK(dht11_interface_init() == 1U);
    CHECK(dht11_interface_write(1U) == 1U);
}

static void begin_response(void) {
    DHT11_PortBegin();
    CHECK(dht11_interface_init() == 0U);
    CHECK(dht11_interface_write(0U) == 0U);
    CHECK(mock_gpio.BSRR == (uint32_t)DHT11_SIG_Pin << 16U);
    dht11_interface_delay_ms(20U);
    CHECK(delayed == 21U && suspends == 0U);
    dht11_interface_disable_irq();
    CHECK(suspends == 1U && resumes == 0U);
    CHECK(dht11_interface_write(1U) == 0U);
    CHECK(mock_gpio.BSRR == DHT11_SIG_Pin);
}

static void test_cleanup_and_gpio(void) {
    reset(); CHECK(DHT11_PortInit()); begin_response();
    uint8_t value = 99U;
    mock_gpio.IDR = DHT11_SIG_Pin;
    CHECK(dht11_interface_read(&value) == 0U && value == 1U);
    mock_gpio.IDR = 0U;
    CHECK(dht11_interface_read(&value) == 0U && value == 0U);
    dht11_interface_enable_irq();
    CHECK(resumes == 0U);
    dht11_interface_disable_irq();
    CHECK(suspends == 1U); /* Close the upstream reset-to-data scheduling gap. */
    registers.CNT += 10U;
    uint32_t elapsed = 0U;
    CHECK(DHT11_PortFinish(&elapsed));
    CHECK(elapsed == 10U && resumes == 1U);
    CHECK(mock_gpio.BSRR == DHT11_SIG_Pin);
    CHECK(DHT11_PortFinish(NULL) && resumes == 1U);
    CHECK(dht11_interface_deinit() == 0U);
    CHECK(registers.CR1 & TIM_CR1_CEN); /* A failed probe cannot stop TIM14. */

    reset(); CHECK(DHT11_PortInit()); begin_response();
    registers.CNT += 16U;
    CHECK(dht11_interface_read(&value) != 0U);
    CHECK(!DHT11_PortFinish(&elapsed));
    CHECK(resumes == 1U && mock_gpio.BSRR == DHT11_SIG_Pin);

    reset(); CHECK(DHT11_PortInit()); begin_response();
    /* LibDriver has an error path before the first bus read. Finish must also
       detect a response budget overrun without another callback. */
    registers.CNT += 6001U;
    CHECK(!DHT11_PortFinish(&elapsed));
    CHECK(elapsed == 6001U && resumes == 1U);
}

static void test_delay_and_wrap(void) {
    reset(); CHECK(DHT11_PortInit());
    registers.CNT = 65530U;
    begin_response();
    increment = 1U;
    dht11_interface_delay_us(40U);
    uint32_t elapsed = 0U;
    CHECK(DHT11_PortFinish(&elapsed));
    CHECK(elapsed >= 41U && elapsed < 55U);
    CHECK(registers.CNT < 100U && resumes == 1U);

    reset(); CHECK(DHT11_PortInit()); begin_response();
    increment = 1U;
    /* A simulated IRQ inside the delay loop must invalidate the frame. */
    spike_read = timer_reads + 2U;
    spike_us = 50U;
    dht11_interface_delay_us(1U);
    CHECK(!DHT11_PortFinish(&elapsed));
    CHECK(resumes == 1U);

    reset(); CHECK(DHT11_PortInit()); begin_response();
    registers.CNT += 16U;
    dht11_interface_delay_us(1U); /* Fault was set before entering busy wait. */
    CHECK(!DHT11_PortFinish(&elapsed));
    CHECK(resumes == 1U);
}

static void test_driver_with_real_port(void) {
    reset(); CHECK(DHT11_PortInit());
    waveform_enabled = true;
    increment = 1U;
    frame[0] = 61U; frame[1] = 0U; frame[2] = 24U; frame[3] = 3U;
    frame[4] = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
    dht11_handle_t sensor = {
        .bus_init = dht11_interface_init,
        .bus_deinit = dht11_interface_deinit,
        .bus_read = dht11_interface_read,
        .bus_write = dht11_interface_write,
        .delay_ms = dht11_interface_delay_ms,
        .delay_us = dht11_interface_delay_us,
        .enable_irq = dht11_interface_enable_irq,
        .disable_irq = dht11_interface_disable_irq,
        .debug_print = dht11_interface_debug_print,
    };
    uint32_t response_us = 0U;
    DHT11_PortBegin();
    CHECK(dht11_init(&sensor) == 0U);
    CHECK(DHT11_PortFinish(&response_us));
    CHECK(sensor.inited == 1U && suspends == 1U && resumes == 1U);

    uint16_t raw_temperature = 0U, raw_humidity = 0U;
    float temperature = 0.0f;
    uint8_t humidity = 0U;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_temperature, &temperature,
                                          &raw_humidity, &humidity) == 0U);
    CHECK(DHT11_PortFinish(&response_us));
    CHECK(temperature > 24.29f && temperature < 24.31f && humidity == 61U);
    CHECK(raw_temperature == ((24U << 8U) | 3U) && raw_humidity == (61U << 8U));
    CHECK(response_us > 3000U && response_us < 6000U);
    CHECK(suspends == 2U && resumes == 2U && mock_gpio.BSRR == DHT11_SIG_Pin);

    frame[4] ^= 1U;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_temperature, &temperature,
                                          &raw_humidity, &humidity) != 0U);
    CHECK(DHT11_PortFinish(&response_us)); /* Protocol error, timing is healthy. */
    CHECK(strstr(DHT11_PortDiagnostic(), "check failed") != NULL);
    CHECK(suspends == 3U && resumes == 3U && mock_gpio.BSRR == DHT11_SIG_Pin);

    frame[4] ^= 1U;
    latency_pending = true;
    DHT11_PortBegin();
    CHECK(dht11_read_temperature_humidity(&sensor, &raw_temperature, &temperature,
                                          &raw_humidity, &humidity) != 0U);
    CHECK(!DHT11_PortFinish(&response_us));
    CHECK(!latency_pending && suspends == 4U && resumes == 4U);
    CHECK(mock_gpio.BSRR == DHT11_SIG_Pin);
}

int main(void) {
    test_clock();
    test_cleanup_and_gpio();
    test_delay_and_wrap();
    test_driver_with_real_port();
    printf("DHT11 STM32 port: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
'''


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    for name, text in {
        "main.h": MAIN_STUB,
        "FreeRTOS.h": RTOS_STUB,
        "task.h": TASK_STUB,
        "port_harness.c": HARNESS,
    }.items():
        (BUILD / name).write_text(text, encoding="utf-8")
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler:
        raise SystemExit("Native GCC is required (or set CC).")
    executable = BUILD / ("test_dht11_port.exe" if os.name == "nt" else "test_dht11_port")
    source = PORT / "driver_dht11_interface.c"
    code = source.read_text(encoding="utf-8")
    forbidden = ("__disable_irq(", "__enable_irq(", "__set_PRIMASK(", "taskENTER_CRITICAL(")
    if any(token in code for token in forbidden):
        raise AssertionError("Port must preserve audio/PPS IRQ delivery.")
    subprocess.run([
        compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(BUILD), "-I", str(DRIVER), "-I", str(PORT),
        str(source), str(DRIVER / "driver_dht11.c"),
        str(BUILD / "port_harness.c"), "-o", str(executable),
    ], check=True, cwd=ROOT)
    subprocess.run([str(executable)], check=True, cwd=ROOT, timeout=10)


if __name__ == "__main__":
    main()
