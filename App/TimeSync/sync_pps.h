#ifndef APP_SYNC_PPS_H__
#define APP_SYNC_PPS_H__

#include <stdbool.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"

bool SyncPps_Init(void);
void SyncPps_Suspend(void);
void SyncPps_Rearm(void);
void SyncPps_Poll(void);
/* Dispatch from the unique HAL callbacks in main.c; TIM6 retains HAL_IncTick. */
void SyncPps_OnPulseFinished(TIM_HandleTypeDef *timer);
void SyncPps_OnPeriodElapsed(TIM_HandleTypeDef *timer);
void SyncPps_Stats(uint32_t *rises, uint32_t *falls, uint32_t *high_seen,
                   uint32_t *low_seen, uint32_t *missed);
#endif
