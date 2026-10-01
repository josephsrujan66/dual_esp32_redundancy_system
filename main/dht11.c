
#include "dht11.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DHT11_START_LOW_MS       20
#define DHT11_RMT_RESOLUTION_HZ  1000000
#define DHT11_SYMBOL_COUNT       64
#define DHT11_RX_TIMEOUT_MS      20

#define DHT11_BIT_THRESHOLD_US   45

static const char *TAG = "DHT11";

static rmt_channel_handle_t rx_channel;
static QueueHandle_t rx_queue;
static int dht_gpio = -1;

static rmt_symbol_word_t rx_symbols[DHT11_SYMBOL_COUNT];

static bool IRAM_ATTR rmt_rx_done_callback(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t *edata,
    void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;

    xQueueSendFromISR(queue, edata, &high_task_wakeup);

    return high_task_wakeup == pdTRUE;
}

esp_err_t dht11_init(int gpio_num)
{
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    dht_gpio = gpio_num;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << dht_gpio,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(dht_gpio, 1);

    rmt_rx_channel_config_t rx_config = {
        .gpio_num = dht_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT11_RMT_RESOLUTION_HZ,
        .mem_block_symbols = DHT11_SYMBOL_COUNT,
        .flags.invert_in = false,
        .flags.with_dma = false
    };

    err = rmt_new_rx_channel(&rx_config, &rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel creation failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (rx_queue == NULL) {
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = rmt_rx_done_callback
    };

    err = rmt_rx_register_event_callbacks(
        rx_channel, &callbacks, rx_queue);

    if (err != ESP_OK) {
        vQueueDelete(rx_queue);
        rx_queue = NULL;
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
        return err;
    }

    err = rmt_enable(rx_channel);
    if (err != ESP_OK) {
        vQueueDelete(rx_queue);
        rx_queue = NULL;
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
        return err;
    }

    ESP_LOGI(TAG, "RMT receiver initialized on GPIO %d", dht_gpio);
    return ESP_OK;
}

esp_err_t dht11_read(dht11_data_t *data)
{
    if (data == NULL || rx_channel == NULL || rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_rx_done_event_data_t rx_event;

    /* Drain any stale completion event. */
    xQueueReset(rx_queue);

    /*
     * Start receiving before releasing the sensor's data line.
     * RMT starts capture on the next input transition.
     */
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 100000
    };

    /*
     * Host start signal: pull DATA low for at least 18 ms.
     */
    gpio_set_direction(dht_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(dht_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(DHT11_START_LOW_MS));

    /*
     * Arm RMT while the line is still low, then release it.
     */
    esp_err_t err = rmt_receive(
        rx_channel,
        rx_symbols,
        sizeof(rx_symbols),
        &receive_config);

    if (err != ESP_OK) {
        gpio_set_level(dht_gpio, 1);
        return err;
    }

    gpio_set_level(dht_gpio, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(dht_gpio, GPIO_MODE_INPUT);

    /*
     * Wait for the complete RMT capture.
     */
    if (xQueueReceive(rx_queue, &rx_event,
                      pdMS_TO_TICKS(DHT11_RX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "RMT receive timed out");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Symbol 0 contains the response/initial transition.
     * Decode the next 40 symbols as the data bits.
     */
    uint8_t raw[5] = {0};

    if (rx_event.num_symbols < 41) {
        ESP_LOGW(TAG, "Incomplete capture: %u symbols",
                 (unsigned)rx_event.num_symbols);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (int bit_index = 0; bit_index < 40; bit_index++) {
        size_t i = (size_t)bit_index + 1;
        rmt_symbol_word_t symbol = rx_event.received_symbols[i];

        uint32_t high_us = symbol.duration0;
        uint32_t low_us = symbol.duration1;

        if (symbol.level0 != 1 || symbol.level1 != 0 ||
            high_us < 10 || high_us > 90 ||
            low_us < 35 || low_us > 80) {
            ESP_LOGW(TAG,
                     "Invalid bit %d: levels=%u/%u, durations=%lu/%lu",
                     bit_index,
                     (unsigned)symbol.level0,
                     (unsigned)symbol.level1,
                     (unsigned long)high_us,
                     (unsigned long)low_us);
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t bit = (high_us > DHT11_BIT_THRESHOLD_US) ? 1 : 0;

        raw[bit_index / 8] <<= 1;
        raw[bit_index / 8] |= bit;
    }

    uint8_t checksum =
        (uint8_t)(raw[0] + raw[1] + raw[2] + raw[3]);

    if (checksum != raw[4]) {
        ESP_LOGW(TAG,
                 "Checksum mismatch: calculated=%u received=%u",
                 checksum, raw[4]);
        return ESP_ERR_INVALID_CRC;
    }

    data->humidity = raw[0];
    data->temperature = raw[2];

    ESP_LOGI(TAG, "DHT11 data: temperature=%u C, humidity=%u%%",
             data->temperature, data->humidity);

    return ESP_OK;

}