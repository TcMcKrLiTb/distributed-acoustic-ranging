#include "sync_hw.h"
#include "sync_pps.h"
#include "sync_port.h"

#define PPS_SECOND_NS 1000000000ULL
#define PPS_ARM_MARGIN_NS 2000000ULL
#define PPS_RECOVERY_NS 100000000ULL

static bool initialized;
static volatile bool armed, high, finished;
static uint64_t target_ns;
static uint32_t poll_ms;
static volatile uint32_t rises, falls, high_seen, low_seen, missed;

static void suspend_locked(void) {
    /* Disconnect the trigger before resetting the timer, including during a
       PTP phase step. Force the output low even if a pulse is in progress. */
    ETH->PTPTSCR &= ~ETH_PTPTSCR_TSITE;
    TIM2->SMCR &= ~TIM_SMCR_SMS;
    TIM2->DIER = 0;
    TIM2->CR1 &= ~TIM_CR1_CEN;
    MODIFY_REG(TIM2->CCMR1, TIM_CCMR1_OC1M, TIM_OCMODE_FORCED_INACTIVE);
    TIM2->SR = 0;
    armed = false;
    high = false;
    finished = false;
}

static void arm_locked(void) {
    suspend_locked();
    TIM2->CNT = 0;
    TIM2->EGR = TIM_EGR_UG; /* Load PSC/CCR preload before waiting for ITR1. */
    TIM2->SR = 0;

    uint64_t now = SyncPort_Time();
    target_ns = (now / PPS_SECOND_NS + 1U) * PPS_SECOND_NS;
    if (target_ns - now < PPS_ARM_MARGIN_NS) target_ns += PPS_SECOND_NS;
    /* Install a future target before acknowledging the old comparator event.
       PTPTSSR is read-to-clear. TSITE self-clears when the target is reached.
       Do not use the HAL/CMSIS TSTTR mask: it describes a different MAC layout
       (bit 5); the F746 GMAC3 status bit is bit 1, per RM0385. */
    ETH->PTPTTHR = (uint32_t)(target_ns / PPS_SECOND_NS);
    ETH->PTPTTLR = 0;
    (void)ETH->PTPTSSR;

    MODIFY_REG(TIM2->CCMR1, TIM_CCMR1_OC1M, TIM_OCMODE_PWM2);
    TIM2->CCER |= TIM_CCER_CC1E;
    TIM2->DIER = TIM_DIER_CC1IE | TIM_DIER_UIE;
    MODIFY_REG(TIM2->SMCR, TIM_SMCR_SMS, TIM_SLAVEMODE_TRIGGER);
    armed = true;
    __DSB();
    ETH->PTPTSCR |= ETH_PTPTSCR_TSITE;
    /* CEN stays zero. Only the ETH target's ITR1 edge may start this pulse. */
}

bool SyncPps_Init(void) {
    initialized = false;
    if (htim2.Instance != TIM2 || TIM2->PSC != 0U ||
        (TIM2->CR1 & (TIM_CR1_DIR | TIM_CR1_CMS)) != 0U ||
        (TIM2->CCMR1 & (TIM_CCMR1_CC1S | TIM_CCMR1_OC1M)) != TIM_OCMODE_PWM2 ||
        (TIM2->CCER & TIM_CCER_CC1P) != 0U ||
        (TIM2->SMCR & (TIM_SMCR_TS | TIM_SMCR_SMS)) !=
            (TIM_TS_ITR1 | TIM_SLAVEMODE_TRIGGER) ||
        (TIM2->OR & TIM_OR_ITR1_RMP) != TIM_TIM2_ETH_PTP) return false;

    uint32_t prescaler = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    uint32_t divider = prescaler < 4U ? 1U : 1U << (prescaler - 3U);
    uint32_t multiplier = (RCC->DCKCFGR1 & RCC_DCKCFGR1_TIMPRE) ?
                          (divider <= 4U ? divider : 4U) : (divider == 1U ? 1U : 2U);
    uint32_t pulse_ticks = HAL_RCC_GetPCLK1Freq() * multiplier / 1000U;
    if (pulse_ticks == 0U) return false;

    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    suspend_locked();
    /* PWM2: high for ARR + 1 - CCR1 ticks. Nonzero CCR1 keeps PA15 low
       while idle; OPM stops on overflow and PWM2 returns low in hardware. */
    TIM2->CR1 |= TIM_CR1_OPM | TIM_CR1_URS;
    TIM2->ARR = pulse_ticks;
    TIM2->CCR1 = 1U;
    rises = falls = high_seen = low_seen = missed = 0;
    poll_ms = SyncPort_Millis();
    initialized = true;
    arm_locked();
    __set_PRIMASK(mask);
    return true;
}

void SyncPps_Suspend(void) {
    if (!initialized) return;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    suspend_locked();
    __set_PRIMASK(mask);
}

void SyncPps_Rearm(void) {
    if (!initialized) return;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    arm_locked();
    __set_PRIMASK(mask);
}

void SyncPps_Poll(void) {
    if (!initialized || !armed) return;
    uint32_t now_ms = SyncPort_Millis();
    if (!finished && now_ms - poll_ms < 2U) return;
    poll_ms = now_ms;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (finished) {
        arm_locked();
    } else if (SyncPort_Time() >= target_ns + PPS_RECOVERY_NS &&
               (TIM2->CR1 & TIM_CR1_CEN) == 0U) {
        /* A missed trigger/interrupt must be visible, and must not permanently
           stop PPS. This is recovery only; it never generates a software edge. */
        missed++;
        arm_locked();
    }
    __set_PRIMASK(mask);
}

void SyncPps_OnPulseFinished(TIM_HandleTypeDef *timer) {
    if (timer->Instance != TIM2 || timer->Channel != HAL_TIM_ACTIVE_CHANNEL_1 ||
        !armed || high) return;
    /* CC1 marks the PWM2 rising edge, not the end of the high pulse. */
    high = true;
    rises++;
    if (GPIOA->IDR & GPIO_PIN_15) high_seen++;
}

void SyncPps_OnPeriodElapsed(TIM_HandleTypeDef *timer) {
    if (timer->Instance != TIM2 || !armed || !high) return;
    /* Overflow already lowered PA15 and cleared CEN through OPM. */
    high = false;
    falls++;
    if ((GPIOA->IDR & GPIO_PIN_15) == 0U) low_seen++;
    finished = true;
}

void SyncPps_Stats(uint32_t *rise_count, uint32_t *fall_count, uint32_t *hi_count,
                   uint32_t *lo_count, uint32_t *missed_count) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    *rise_count = rises; *fall_count = falls;
    *hi_count = high_seen; *lo_count = low_seen; *missed_count = missed;
    __set_PRIMASK(mask);
}
