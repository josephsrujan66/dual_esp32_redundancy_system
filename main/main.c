#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "dht11.h"
#include "wifi_manager.h"
#include "sensor_data.h"
#include "network_task.h"
#include "redundancy.h"

#define DHT11_GPIO 4
#define SENSOR_INTERVAL_MS 30000
static const char *TAG = "MAIN";

static void sensor_task(void *arg)
{
    dht11_data_t dht_data;

    ESP_LOGI(TAG, "Sensor task started");
    ESP_LOGI(TAG, "Waiting 2 seconds before first DHT11 read");

    /* Give DHT11 time to stabilize */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {

        esp_err_t err = dht11_read(&dht_data);

        if (err == ESP_OK) {

            ESP_LOGI(TAG,
                     "Temperature: %u C | Humidity: %u%%",
                     dht_data.temperature,
                     dht_data.humidity);

            /* Update shared sensor data */
            sensor_data_write(
                dht_data.temperature,
                dht_data.humidity
            );

        } else {

            ESP_LOGE(TAG,
                     "DHT11 read failed: %s",
                     esp_err_to_name(err));
        }

        /*
         * DHT11 should not be polled frequently.
         * Wait 30 seconds before the next reading.
         */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Dual ESP32 redundancy system starting");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize DHT11 */
    ESP_ERROR_CHECK(dht11_init(DHT11_GPIO));

    /* Start Wi-Fi */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* Wait until Wi-Fi is connected */
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Wi-Fi ready");

    ESP_ERROR_CHECK(redundancy_init());

    ESP_LOGI(TAG, "Waiting for DHT11 startup...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    sensor_data_init();

    ESP_LOGI(TAG, "Starting sensor task");

    /* Start sensor task */
    BaseType_t result = xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
    }
    network_task_start();

}