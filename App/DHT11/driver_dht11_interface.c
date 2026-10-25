#include "driver_dht11_interface.h"
#include "dht11_port.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>

extern TIM_HandleTypeDef htim14;

#define TIMER_HZ 1000000U
#define MAX_CALLBACK_GAP_US 15U
#define MAX_RESPONSE_US 6000U

static bool ready, scheduler_held, timing_fault, tracking;
static uint16_t response_start, last_callback;
/* LibDriver can report an error from within its timing callbacks. Buffer it;
   formatting here never waits for a queue or UART, and IRQ masks are untouched. */
static char diagnostic[96];

static uint16_t counter(void) { return (uint16_t)TIM14->CNT; }

static void check_gap(uint16_t now) {
    if (tracking && ((uint16_t)(now - last_callback) > MAX_CALLBACK_GAP_US ||
                     (uint16_t)(now - response_start) > MAX_RESPONSE_US))
        timing_fault = true;
}

bool DHT11_PortInit(void) {
    ready = false;
    if (htim14.Instance != TIM14 || (TIM14->CR1 & TIM_CR1_CEN) != 0U)
        return false;

    uint32_t prescaler = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    uint32_t divider = prescaler < 4U ? 1U : 1U << (prescaler - 3U);
    uint32_t multiplier = (RCC->DCKCFGR1 & RCC_DCKCFGR1_TIMPRE) ?
        (divider <= 4U ? divider : 4U) : (divider == 1U ? 1U : 2U);
    uint32_t timer_hz = HAL_RCC_GetPCLK1Freq() * multiplier;
    if (timer_hz < TIMER_HZ || timer_hz % TIMER_HZ != 0U ||
        timer_hz / TIMER_HZ > 65536U) return false;

    /* Own TIM14 as a 16-bit, 1 us free-running clock. No PWM/IRQ start calls.
       Derive PSC from the real APB1/TIMPRE clocks instead of assuming 108 MHz. */
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

    /* Open drain: writing one releases the bus to the external 10 kohm pull-up.
       IDR remains readable in output mode, so no GPIO mode switching is needed. */
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    ready = true;
    return true;
}

void DHT11_PortBegin(void) {
    timing_fault = tracking = false;
    diagnostic[0] = '\0';
}

bool DHT11_PortFinish(uint32_t *response_us) {
    uint32_t elapsed = tracking ? (uint16_t)(counter() - response_start) : 0U;
    bool valid = !timing_fault && elapsed <= MAX_RESPONSE_US;
    tracking = false;
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    if (response_us) *response_us = elapsed;
    if (scheduler_held) {
        scheduler_held = false;
        (void)xTaskResumeAll();
    }
    return valid;
}

const char *DHT11_PortDiagnostic(void) { return diagnostic; }

uint8_t dht11_interface_init(void) {
    if (!ready) return 1U;
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    return 0U;
}

uint8_t dht11_interface_deinit(void) {
    /* The application owns the running timer; a failed sensor probe must not
       stop the timebase needed by the next automatic retry. */
    DHT11_SIG_GPIO_Port->BSRR = DHT11_SIG_Pin;
    return 0U;
}

uint8_t dht11_interface_read(uint8_t *value) {
    uint16_t now = counter();
    check_gap(now);
    if (!value || !ready || timing_fault) return 1U;
    *value = (DHT11_SIG_GPIO_Port->IDR & DHT11_SIG_Pin) != 0U;
    last_callback = counter();
    if ((uint16_t)(last_callback - now) > MAX_CALLBACK_GAP_US) timing_fault = true;
    return timing_fault ? 1U : 0U;
}

uint8_t dht11_interface_write(uint8_t value) {
    if (!ready) return 1U;
    if (value && scheduler_held) {
        response_start = last_callback = counter();
        tracking = true;
    }
    DHT11_SIG_GPIO_Port->BSRR = value ? DHT11_SIG_Pin : (uint32_t)DHT11_SIG_Pin << 16U;
    check_gap(counter());
    return timing_fault ? 1U : 0U;
}

void dht11_interface_delay_ms(uint32_t ms) {
    /* Only the 20 ms start pulse uses this callback. Keep all tasks runnable
       during it; an extra tick ensures the minimum despite tick phase. */
    configASSERT(!scheduler_held);
    vTaskDelay(pdMS_TO_TICKS(ms) + 1U);
}

void dht11_interface_delay_us(uint32_t us) {
    uint16_t start = counter();
    check_gap(start);
    /* Add one tick because entry can occur just before the next counter edge.
       Splitting keeps subtraction safe across the 16-bit counter's wrap. */
    while (us != 0U && !timing_fault) {
        uint16_t duration = (uint16_t)(us > 1000U ? 1000U : us);
        uint16_t elapsed;
        do { elapsed = (uint16_t)(counter() - start); }
        while (elapsed < duration + 1U);
        if (elapsed > duration + 1U + MAX_CALLBACK_GAP_US) timing_fault = true;
        us -= duration;
        last_callback = (uint16_t)(start + elapsed);
        start = counter();
        check_gap(start);
    }
    if (us == 0U) last_callback = start;
}

void dht11_interface_disable_irq(void) {
    /* Preserve hardware IRQs for audio DMA, PPS and both millisecond clocks.
       Suspend task switching only. Upstream re-enables between handshake and
       payload; retain ownership across that gap until DHT11_PortFinish. */
    if (!scheduler_held) {
        vTaskSuspendAll();
        scheduler_held = true;
    }
}

void dht11_interface_enable_irq(void) {
    /* Scheduling is restored unconditionally by DHT11_PortFinish. */
}

void dht11_interface_debug_print(const char *const fmt, ...) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(diagnostic, sizeof(diagnostic), fmt, args);
    va_end(args);
    for (unsigned i = 0U; diagnostic[i] != '\0'; i++)
        if (diagnostic[i] == '\r' || diagnostic[i] == '\n') diagnostic[i] = ' ';
}
