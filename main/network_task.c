#include "network_task.h"

#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"

#include "sensor_data.h"
#include "redundancy.h"

#define NETWORK_TASK_INTERVAL_MS 2000

#define MQTT_BROKER_URI "mqtt://broker.emqx.io"
#define MQTT_TOPIC      "dual_esp32_redundancy/telemetry"

#define MQTT_CONNECTED_BIT BIT0

static const char *TAG = "NETWORK_TASK";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static EventGroupHandle_t mqtt_event_group = NULL;


/* ---------------------------------------------------------
 * MQTT event handler
 * --------------------------------------------------------- */
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        xEventGroupClearBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT connection error");
        break;

    default:
        break;
    }
}


/* ---------------------------------------------------------
 * MQTT initialization
 * --------------------------------------------------------- */
static esp_err_t mqtt_init(void)
{
    mqtt_event_group = xEventGroupCreate();

    if (mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT event group");
        return ESP_ERR_NO_MEM;
    }

    char client_id[32];

    snprintf(client_id, sizeof(client_id),
             "dual_esp32_node_%s",
             CONFIG_REDUNDANCY_NODE_ID);

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = client_id,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);

    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler");
        return err;
    }

    err = esp_mqtt_client_start(mqtt_client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started");
    ESP_LOGI(TAG, "Broker: %s", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "Topic: %s", MQTT_TOPIC);

    return ESP_OK;
}


/* ---------------------------------------------------------
 * Network task
 * --------------------------------------------------------- */
static void network_task(void *arg)
{
    (void)arg;

    sensor_data_t data;
    char payload[128];

    ESP_LOGI(TAG, "Network task started");

    while (1) {

        EventBits_t bits = xEventGroupGetBits(mqtt_event_group);

        if (!(bits & MQTT_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "MQTT not connected");
            vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_INTERVAL_MS));
            continue;
        }

        /*
         * Only the ACTIVE node is allowed to publish.
         */
        if (!redundancy_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_INTERVAL_MS));
            continue;
        }

        if (!sensor_data_read(&data)) {
            ESP_LOGE(TAG, "Failed to read shared sensor data");
            vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_INTERVAL_MS));
            continue;
        }

        if (!data.valid) {
            ESP_LOGW(TAG, "Sensor data not available yet");
            vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_INTERVAL_MS));
            continue;
        }

        snprintf(payload, sizeof(payload),
                 "{\"node_id\":\"%s\","
                 "\"temperature\":%u,"
                 "\"humidity\":%u,"
                 "\"sequence\":%lu}",
                 CONFIG_REDUNDANCY_NODE_ID,
                 data.temperature,
                 data.humidity,
                 (unsigned long)data.sequence);

        int message_id = esp_mqtt_client_publish(
            mqtt_client,
            MQTT_TOPIC,
            payload,
            0,
            1,
            0
        );

        if (message_id >= 0) {
            ESP_LOGI(TAG, "Published: %s", payload);
        } else {
            ESP_LOGE(TAG, "MQTT publish failed");
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_INTERVAL_MS));
    }
}


/* ---------------------------------------------------------
 * Start network task
 * --------------------------------------------------------- */
void network_task_start(void)
{
    esp_err_t err = mqtt_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT initialization failed");
        return;
    }

    BaseType_t result = xTaskCreate(
        network_task,
        "network_task",
        4096,
        NULL,
        5,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network task");
    }
}
