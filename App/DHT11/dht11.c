#include "dht11.h"
#include "dht11_port.h"
#include "driver_dht11_interface.h"
#include "TimeSync/Port/sync_log.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SENSOR_WARMUP_MS 2000U
#define SAMPLE_PERIOD_MS 2000U
#define SENSOR_PROBE_MS 5000U
#define SENSOR_FAILURE_LIMIT 3U
#define SAMPLE_MAX_AGE_MS 3500U
#define SENSOR_STACK_WORDS 768U

static dht11_handle_t sensor;
static DHT11Status published;
static TaskHandle_t worker;
static StaticTask_t worker_control;
static StackType_t worker_stack[SENSOR_STACK_WORDS];
static uint8_t initialization_frame[5];
static bool capturing_initialization, initialization_frame_valid;

/* LibDriver initialization already requests one complete wire frame. Reuse it
   once through the normal read API for checksum/conversion, without immediately
   issuing a second request to the sensor. This also publishes recovery probes
   as real samples instead of waiting for another sampling interval. */
static uint8_t read_frame(uint8_t data[5]) {
    if (initialization_frame_valid) {
        memcpy(data, initialization_frame, sizeof(initialization_frame));
        initialization_frame_valid = false;
        return 0U;
    }
    uint8_t result = dht11_interface_read_frame(data);
    if (result == 0U && capturing_initialization) {
        memcpy(initialization_frame, data, sizeof(initialization_frame));
        initialization_frame_valid = true;
    }
    return result;
}

static void publish(const DHT11Status *status) {
    taskENTER_CRITICAL();
    published = *status;
    taskEXIT_CRITICAL();
}

static void sensor_task(void *argument) {
    (void)argument;
    DHT11Status status = {.last_error = 6U};
    vTaskDelay(pdMS_TO_TICKS(SENSOR_WARMUP_MS) + 1U);
    for (;;) {
        bool probing = sensor.inited != 1U;
        float temperature = 0.0f;
        uint8_t humidity = 0U;
        uint16_t temperature_raw = 0U, humidity_raw = 0U;
        DHT11_PortBegin();
        uint8_t result = 0U;
        if (probing) {
            initialization_frame_valid = false;
            capturing_initialization = true;
            result = dht11_init(&sensor);
            capturing_initialization = false;
        }
        if (result == 0U)
            result = dht11_read_temperature_humidity(&sensor, &temperature_raw, &temperature,
                                                     &humidity_raw, &humidity);
        initialization_frame_valid = false;
        bool timing_ok = DHT11_PortFinish(&status.response_us);
        status.edge_count = DHT11_PortEdges();
        if (!timing_ok) {
            result = 5U;
            if (probing) sensor.inited = 0U;
        }
        status.attempts++;
        status.initialized = sensor.inited == 1U;
        status.last_error = result;
        status.timing_error = !timing_ok;
        status.valid = result == 0U;
        if (status.valid) {
            status.present = true;
            status.consecutive_failures = 0U;
            status.temperature_c_x10 = (int16_t)(temperature * 10.0f +
                                                (temperature < 0.0f ? -0.5f : 0.5f));
            status.humidity_percent = humidity;
            status.temperature_raw = temperature_raw;
            status.humidity_raw = humidity_raw;
            status.sampled_ms = HAL_GetTick();
            status.samples++;
        } else {
            status.failures++;
            if (status.consecutive_failures < UINT8_MAX) status.consecutive_failures++;
            if (status.consecutive_failures >= SENSOR_FAILURE_LIMIT) status.present = false;
        }
        publish(&status);

        uint32_t interval_ms = !status.present && status.consecutive_failures >= SENSOR_FAILURE_LIMIT ?
                               SENSOR_PROBE_MS : SAMPLE_PERIOD_MS;
        if (!status.valid) {
            char line[224];
            snprintf(line, sizeof(line), "DHT11 error=%u timing=%u edges=%u consecutive=%u present=%u"
                     " response_us=%" PRIu32 " retry_ms=%" PRIu32 " %s\r\n",
                     result, status.timing_error ? 1U : 0U, status.edge_count,
                     status.consecutive_failures, status.present ? 1U : 0U,
                     status.response_us, interval_ms, DHT11_PortDiagnostic());
            SyncLog_Write(line);
        }
        /* The connected DHT11 modules require a full 2 s conversion interval
           after a transaction. The application refreshes this cache at 1 Hz. */
        vTaskDelay(pdMS_TO_TICKS(interval_ms) + 1U);
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
    DRIVER_DHT11_LINK_READ_FRAME(&sensor, read_frame);
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
