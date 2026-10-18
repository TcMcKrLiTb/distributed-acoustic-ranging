#ifndef APP_SYNC_HW_H__
#define APP_SYNC_HW_H__

#include "main.h"

/* CubeMX keeps the peripheral definitions in main.c (CoupleFile=false). */
extern ETH_HandleTypeDef heth;
extern TIM_HandleTypeDef htim2;
extern UART_HandleTypeDef huart1;
extern RNG_HandleTypeDef hrng;

#endif