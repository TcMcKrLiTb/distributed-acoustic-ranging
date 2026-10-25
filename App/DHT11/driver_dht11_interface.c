#include "driver_dht11_interface.h"
#include "dht11_port.h"
#include "dht11_capture.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>

extern TIM_HandleTypeDef htim14;

#define TIMER_HZ 1000000U
#define FRAME_EDGES 84U /* Host release + 3 response edges + 40 rise/fall pairs. */
#define FRAME_TIMEOUT_MS 10U
#define EXTI_IRQ_PRIORITY 3U /* Above audio/display (5) and HAL timebase (4). */

typedef struct { uint16_t us; bool high; } Edge;
static volatile Edge edges[FRAME_EDGES];
static volatile uint8_t edge_count;
static volatile bool active, frame_done;
static bool ready, timing_fault;
static uint32_t response_us;
static char diagnostic[96];

static uint16_t counter(void) { return (uint16_t)TIM14->CNT; }

static void stop_capture(void) {
    EXTI->IMR &= ~(uint32_t)DHT11_SIG_Pin;
    __DSB();
    active = false;
    EXTI->PR = DHT11_SIG_Pin;
}

/* The startup file supplies a weak vector, so this survives CubeMX regeneration.
   This module owns EXTI6 and the otherwise-unused EXTI9_5 vector. No RTOS calls
   are allowed here: priority 3 is above the FreeRTOS syscall priority ceiling. */
void EXTI9_5_IRQHandler(void) {
    if ((EXTI->PR & DHT11_SIG_Pin) == 0U) return;
    uint16_t now = counter();
    bool high = (DHT11_SIG_GPIO_Port->IDR & DHT11_SIG_Pin) != 0U;
    EXTI->PR = DHT11_SIG_Pin;
    if (!active || (EXTI->IMR & DHT11_SIG_Pin) == 0U) return;
    uint8_t count = edge_count;
    if (count < FRAME_EDGES) {
        edges[count].us = now;
        edges[count].high = high;
        edge_count = ++count;
    }
    if (count == FRAME_EDGES) {
        EXTI->IMR &= ~(uint32_t)DHT11_SIG_Pin;
        active = false;
        __DMB();
        frame_done = true;
    }
}

bool DHT11_PortInit(void) {
    ready = false;
    if (htim14.Instance != TIM14 || (TIM14->CR1 & TIM_CR1_CEN) != 0U)
        return false;
    /* Do not take over another user's line in the shared vector. */
    if ((EXTI->IMR & (GPIO_PIN_5 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9)) != 0U)
        return false;

    uint32_t prescaler = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    uint32_t divider = prescaler < 4U ? 1U : 1U << (prescaler - 3U);
    uint32_t multiplier = (RCC->DCKCFGR1 & RCC_DCKCFGR1_TIMPRE) ?
        (divider <= 4U ? divider : 4U) : (divider == 1U ? 1U : 2U);
    uint32_t timer_hz = HAL_RCC_GetPCLK1Freq() * multiplier;
    if (timer_hz < TIMER_HZ || timer_hz % TIMER_HZ != 0U ||
        timer_hz / TIMER_HZ > 65536U) return false;

    htim14.Init.Prescaler = timer_hz / TIMER_HZ - 1U;
    htim14.Init.Period = UINT16_MAX;
    htim14.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim14.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim14.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim14) != HAL_OK) return false;
    TIM14->DIER = 0U;
    TIM14->CCER = 0U;
    TIM14->CR1 &= ~TIM_CR1_OPM;
    TIM14->EGR = TIM_EGR_UG;
    TIM14->SR = 0U;
    TIM14->CNT = 0U;
    if (HAL_TIM_Base_Start(&htim14) != HAL_OK) return false;

    /* Output-open-drain mode retains the digital input buffer. Configure the
       EXTI route directly so PG6 can pull low without changing GPIO MODER. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    stop_capture();
    MODIFY_REG(SYSCFG->EXTICR[1], SYSCFG_EXTICR2_EXTI6, SYSCFG_EXTICR2_EXTI6_PG);
    EXTI->EMR &= ~(uint32_t)DHT11_SIG_Pin;
    EXTI->RTSR |= DHT11_SIG_Pin;
    EXTI->FTSR |= DHT11_SIG_Pin;
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, EXTI_IRQ_PRIORITY, 0U);
    HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    ready = true;
    return true;
}

void DHT11_PortBegin(void) {
    timing_fault = false;
    response_us = 0U;
    edge_count = 0U;
    diagnostic[0] = '\0';
}

bool DHT11_PortFinish(uint32_t *elapsed_us) {
    stop_capture();
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    if (elapsed_us) *elapsed_us = response_us;
    return !timing_fault;
}

const char *DHT11_PortDiagnostic(void) { return diagnostic; }
uint8_t DHT11_PortEdges(void) { return edge_count; }

uint8_t dht11_interface_read_frame(uint8_t data[5]) {
    if (!ready || !data) return 1U;
    stop_capture();
    edge_count = 0U;
    frame_done = false;
    DHT11_SIG_GPIO_Port->BSRR = (uint32_t)DHT11_SIG_Pin << 16U;
    dht11_interface_delay_ms(20U);

    DHT11Capture capture;
    DHT11Capture_Begin(&capture, counter());
    EXTI->PR = DHT11_SIG_Pin;
    active = true;
    __DMB();
    EXTI->IMR |= DHT11_SIG_Pin;
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    uint32_t started_ms = HAL_GetTick();
    while (!frame_done && HAL_GetTick() - started_ms < FRAME_TIMEOUT_MS)
        vTaskDelay(1U);
    stop_capture();
    __DMB();

    uint8_t count = edge_count;
    for (unsigned i = 0U; i < count; i++)
        DHT11Capture_Edge(&capture, edges[i].us, edges[i].high);
    DHT11Capture_Timeout(&capture, counter());
    response_us = capture.response_us;
    if (capture.phase != DHT11_CAPTURE_DONE) {
        /* A missing module (only the host release edge) is an ordinary timeout.
           Invalid/incomplete sensor pulses are a timing/capture failure. */
        timing_fault = capture.error != DHT11_CAPTURE_TIMEOUT || count > 1U;
        snprintf(diagnostic, sizeof(diagnostic), "dht11: capture error=%u edges=%u bits=%u",
                 (unsigned)capture.error, count, capture.bits);
        return 1U;
    }
    for (unsigned i = 0U; i < 5U; i++) data[i] = capture.data[i];
    return 0U;
}

uint8_t dht11_interface_init(void) {
    if (!ready) return 1U;
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    return 0U;
}

uint8_t dht11_interface_deinit(void) {
    stop_capture();
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    return 0U;
}

/* Legacy callback bindings are retained for the LibDriver handle checks. The
   STM32 task uses read_frame, which captures edges while all tasks keep running. */
uint8_t dht11_interface_read(uint8_t *value) {
    if (!value || !ready) return 1U;
    *value = (DHT11_SIG_GPIO_Port->IDR & DHT11_SIG_Pin) != 0U;
    return 0U;
}

uint8_t dht11_interface_write(uint8_t value) {
    if (!ready) return 1U;
    DHT11_SIG_GPIO_Port->BSRR = value ? DHT11_SIG_Pin : (uint32_t)DHT11_SIG_Pin << 16U;
    return 0U;
}

void dht11_interface_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1U); }

void dht11_interface_delay_us(uint32_t us) {
    while (us != 0U) {
        uint16_t duration = (uint16_t)(us > 1000U ? 1000U : us);
        uint16_t start = counter();
        while ((uint16_t)(counter() - start) < duration + 1U) { }
        us -= duration;
    }
}

void dht11_interface_disable_irq(void) { }
void dht11_interface_enable_irq(void) { }

void dht11_interface_debug_print(const char *const fmt, ...) {
    if (diagnostic[0] != '\0') return; /* Preserve the first, specific cause. */
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(diagnostic, sizeof(diagnostic), fmt, args);
    va_end(args);
    for (unsigned i = 0U; diagnostic[i] != '\0'; i++)
        if (diagnostic[i] == '\r' || diagnostic[i] == '\n') diagnostic[i] = ' ';
}
