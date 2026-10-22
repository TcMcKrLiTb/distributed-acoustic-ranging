#include "sync_log.h"
#include "sync_hw.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <string.h>

#define LOG_LENGTH 384U
#define LOG_DEPTH 4U

typedef struct {
    uint16_t length;
    char text[LOG_LENGTH];
} LogMessage;

static StaticQueue_t queue_control;
static uint8_t queue_storage[LOG_DEPTH * sizeof(LogMessage)];
static QueueHandle_t volatile log_queue;
static uint32_t dropped;

void SyncLog_Init(void) {
    dropped = 0;
    log_queue = xQueueCreateStatic(LOG_DEPTH, sizeof(LogMessage), queue_storage,
                                   &queue_control);
}

void SyncLog_Write(const char *text) {
    LogMessage message = {0};
    if (!text) return;
    while (message.length < LOG_LENGTH - 1U && text[message.length])
        message.length++;
    memcpy(message.text, text, message.length);
    if (!log_queue || xQueueSend(log_queue, &message, 0) != pdPASS) {
        taskENTER_CRITICAL();
        dropped++;
        taskEXIT_CRITICAL();
    }
}

void SyncLog_Poll(void) {
    /* The default task has a 256-word stack; keep the UART buffer in BSS. */
    static LogMessage message;
    QueueHandle_t queue = log_queue;
    if (queue && xQueueReceive(queue, &message, 0) == pdPASS) {
        if (HAL_UART_Transmit(&huart1, (uint8_t *)message.text, message.length, 100U) != HAL_OK) {
            taskENTER_CRITICAL();
            dropped++;
            taskEXIT_CRITICAL();
        }
    }
}

uint32_t SyncLog_Dropped(void) {
    uint32_t value;
    taskENTER_CRITICAL();
    value = dropped;
    taskEXIT_CRITICAL();
    return value;
}
