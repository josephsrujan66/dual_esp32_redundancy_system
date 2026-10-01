#include "sensor_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static sensor_data_t shared_data = {
    .temperature = 25,
    .humidity = 35,
    .valid = false,
    .sequence = 0
};

static SemaphoreHandle_t data_mutex = NULL;

void sensor_data_init(void)
{
    data_mutex = xSemaphoreCreateMutex();
}

void sensor_data_write(uint8_t temperature,
                       uint8_t humidity)
{
    if (data_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {

        shared_data.temperature = temperature;
        shared_data.humidity = humidity;
        shared_data.valid = true;
        shared_data.sequence++;

        xSemaphoreGive(data_mutex);
    }
}

bool sensor_data_read(sensor_data_t *data)
{
    if (data == NULL || data_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {

        *data = shared_data;

        xSemaphoreGive(data_mutex);

        return true;
    }

    return false;
}