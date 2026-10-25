#include "dht11.h"
#include "dht11_port.h"
#include "driver_dht11_interface.h"
#include "TimeSync/Port/sync_log.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <inttypes.h>
#include <stdio.h>

#define SAMPLE_PERIOD_MS 2000U
#define SAMPLE_MAX_AGE_MS 6000U
#define SENSOR_STACK_WORDS 768U

static dht11_handle_t sensor;
static DHT11Status published;
static TaskHandle_t worker;
static StaticTask_t worker_control;
static StackType_t worker_stack[SENSOR_STACK_WORDS];

static void publish(const DHT11Status *status) {
    taskENTER_CRITICAL();
    published = *status;
    taskEXIT_CRITICAL();
}

static void sensor_task(void *argument) {
    (void)argument;
    DHT11Status status = {.last_error = 6U};
    /* DHT11 must settle after power-up. Init itself requests a frame, so the
       first readable sample is a further 2 s after a successful probe. */
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS) + 1U);
    for (;;) {
        bool probing = sensor.inited != 1U;
        float temperature = 0.0f;
        uint8_t humidity = 0U;
        uint16_t temperature_raw = 0U, humidity_raw = 0U;
        DHT11_PortBegin();
        uint8_t result = probing ? dht11_init(&sensor) :
            dht11_read_temperature_humidity(&sensor, &temperature_raw, &temperature,
                                            &humidity_raw, &humidity);
        bool timing_ok = DHT11_PortFinish(&status.response_us);
        if (!timing_ok) {
            result = 5U;
            if (probing) sensor.inited = 0U;
        }
        status.attempts++;
        status.initialized = sensor.inited == 1U;
        status.last_error = result;
        status.timing_error = !timing_ok;
        status.valid = result == 0U && !probing;
        if (status.valid) {
            status.temperature_c_x10 = (int16_t)(temperature * 10.0f +
                                                (temperature < 0.0f ? -0.5f : 0.5f));
            status.humidity_percent = humidity;
            status.temperature_raw = temperature_raw;
            status.humidity_raw = humidity_raw;
            status.sampled_ms = HAL_GetTick();
            status.samples++;
        } else if (result != 0U) {
            status.failures++;
        }
        publish(&status);

        char line[224];
        if (status.valid) {
            /* Integer output also works with newlib-nano without printf-float. */
            snprintf(line, sizeof(line), "DHT11 temp_x10=%d humidity=%u%% response_us=%" PRIu32
                     " samples=%" PRIu32 " failures=%" PRIu32 "\r\n",
                     status.temperature_c_x10, status.humidity_percent, status.response_us,
                     status.samples, status.failures);
        } else {
            snprintf(line, sizeof(line), "DHT11 %s error=%u timing=%u response_us=%" PRIu32 " %s\r\n",
                     result == 0U ? "ready" : "retry", result,
                     status.timing_error ? 1U : 0U, status.response_us, DHT11_PortDiagnostic());
        }
        SyncLog_Write(line);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS) + 1U);
    }
}

void DHT11_Init(void) {
    if (worker) return;
    published.last_error = 6U;
    if (!DHT11_PortInit()) {
        published.last_error = 5U;
        return;
    }
    DRIVER_DHT11_LINK_INIT(&sensor, dht11_handle_t);
    DRIVER_DHT11_LINK_BUS_INIT(&sensor, dht11_interface_init);
    DRIVER_DHT11_LINK_BUS_DEINIT(&sensor, dht11_interface_deinit);
    DRIVER_DHT11_LINK_BUS_READ(&sensor, dht11_interface_read);
    DRIVER_DHT11_LINK_BUS_WRITE(&sensor, dht11_interface_write);
    DRIVER_DHT11_LINK_DELAY_MS(&sensor, dht11_interface_delay_ms);
    DRIVER_DHT11_LINK_DELAY_US(&sensor, dht11_interface_delay_us);
    DRIVER_DHT11_LINK_DISABLE_IRQ(&sensor, dht11_interface_disable_irq);
    DRIVER_DHT11_LINK_ENABLE_IRQ(&sensor, dht11_interface_enable_irq);
    DRIVER_DHT11_LINK_DEBUG_PRINT(&sensor, dht11_interface_debug_print);
    worker = xTaskCreateStatic(sensor_task, "DHT11", SENSOR_STACK_WORDS, NULL,
                               tskIDLE_PRIORITY + 1U, worker_stack, &worker_control);
    if (!worker) published.last_error = 5U;
}

bool DHT11_GetLatest(DHT11Status *status) {
    if (!status) return false;
    taskENTER_CRITICAL();
    *status = published;
    taskEXIT_CRITICAL();
    status->valid = status->valid && HAL_GetTick() - status->sampled_ms <= SAMPLE_MAX_AGE_MS;
    return status->valid;
}
