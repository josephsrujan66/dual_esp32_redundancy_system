#include "redundancy.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "REDUNDANCY";

#define HEARTBEAT_MAGIC       0xD711A55A
#define STARTUP_GRACE_MS      3000
#define PEER_TIMEOUT_MS       2000
#define STATE_TASK_INTERVAL_MS 100

typedef enum {
    NODE_INIT = 0,
    NODE_ACTIVE,
    NODE_REST
} node_state_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint8_t node_id;
    uint8_t state;
} heartbeat_msg_t;

static uint8_t peer_mac[ESP_NOW_ETH_ALEN];
static uint32_t heartbeat_sequence = 0;

static node_state_t current_state = NODE_INIT;
static node_state_t peer_state = NODE_INIT;

static bool peer_seen = false;
static TickType_t last_peer_heartbeat = 0;
static TickType_t startup_time = 0;

static portMUX_TYPE redundancy_mux = portMUX_INITIALIZER_UNLOCKED;


/* ---------------------------------------------------------
 * Node identity
 * --------------------------------------------------------- */
static uint8_t get_node_id(void)
{
    return (uint8_t)CONFIG_REDUNDANCY_NODE_ID[0];
}


/* ---------------------------------------------------------
 * State name
 * --------------------------------------------------------- */
static const char *state_name(node_state_t state)
{
    switch (state) {
    case NODE_INIT:
        return "INIT";
    case NODE_ACTIVE:
        return "ACTIVE";
    case NODE_REST:
        return "REST";
    default:
        return "UNKNOWN";
    }
}


/* ---------------------------------------------------------
 * Change local state
 * --------------------------------------------------------- */
static void set_state(node_state_t new_state)
{
    node_state_t old_state;

    taskENTER_CRITICAL(&redundancy_mux);
    old_state = current_state;
    current_state = new_state;
    taskEXIT_CRITICAL(&redundancy_mux);

    if (old_state != new_state) {
        ESP_LOGI(TAG, "State transition: %s -> %s",
                 state_name(old_state),
                 state_name(new_state));
    }
}


/* ---------------------------------------------------------
 * Get local state safely
 * --------------------------------------------------------- */
static node_state_t get_state(void)
{
    node_state_t state;

    taskENTER_CRITICAL(&redundancy_mux);
    state = current_state;
    taskEXIT_CRITICAL(&redundancy_mux);

    return state;
}

/* ---------------------------------------------------------
 * Verify local state safely
 * --------------------------------------------------------- */
bool redundancy_is_active(void)
{
    return get_state() == NODE_ACTIVE;
}

/* ---------------------------------------------------------
 * Print MAC address
 * --------------------------------------------------------- */
static void print_mac(const uint8_t *mac)
{
    ESP_LOGI(TAG, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}


/* ---------------------------------------------------------
 * Parse MAC address from menuconfig string
 * --------------------------------------------------------- */
static esp_err_t parse_peer_mac(const char *mac_string,
                                uint8_t *mac)
{
    unsigned int values[6];

    if (mac_string == NULL || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int count = sscanf(mac_string,
                       "%2x:%2x:%2x:%2x:%2x:%2x",
                       &values[0],
                       &values[1],
                       &values[2],
                       &values[3],
                       &values[4],
                       &values[5]);

    if (count != 6) {
        ESP_LOGE(TAG, "Invalid peer MAC format: %s", mac_string);
        ESP_LOGE(TAG, "Expected format: AA:BB:CC:DD:EE:FF");
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 6; i++) {
        if (values[i] > 0xFF) {
            ESP_LOGE(TAG, "Invalid MAC byte %d: %u", i, values[i]);
            return ESP_ERR_INVALID_ARG;
        }

        mac[i] = (uint8_t)values[i];
    }

    return ESP_OK;
}


/* ---------------------------------------------------------
 * ESP-NOW send callback
 * --------------------------------------------------------- */
static void send_callback(const esp_now_send_info_t *tx_info,
                          esp_now_send_status_t status)
{
    (void)tx_info;

    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Heartbeat send failed");
    }
}


/* ---------------------------------------------------------
 * ESP-NOW receive callback
 * --------------------------------------------------------- */
static void recv_callback(const esp_now_recv_info_t *recv_info,
                          const uint8_t *data,
                          int len)
{
    if (len != sizeof(heartbeat_msg_t)) {
        ESP_LOGW(TAG, "Invalid packet length: %d", len);
        return;
    }

    if (memcmp(recv_info->src_addr, peer_mac, ESP_NOW_ETH_ALEN) != 0) {
        ESP_LOGW(TAG, "Heartbeat received from unexpected MAC");
        return;
    }

    heartbeat_msg_t msg;
    memcpy(&msg, data, sizeof(msg));

    if (msg.magic != HEARTBEAT_MAGIC ||
        msg.state > NODE_REST) {
        ESP_LOGW(TAG, "Invalid heartbeat packet");
        return;
    }

    taskENTER_CRITICAL(&redundancy_mux);
    peer_seen = true;
    peer_state = (node_state_t)msg.state;
    last_peer_heartbeat = xTaskGetTickCount();
    taskEXIT_CRITICAL(&redundancy_mux);
}


/* ---------------------------------------------------------
 * Heartbeat task
 * --------------------------------------------------------- */
static void heartbeat_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Heartbeat task started");
    ESP_LOGI(TAG, "Heartbeat interval: %d ms",
             CONFIG_REDUNDANCY_HEARTBEAT_INTERVAL_MS);

    while (1) {
        heartbeat_msg_t msg = {0};

        msg.magic = HEARTBEAT_MAGIC;
        msg.sequence = heartbeat_sequence++;
        msg.node_id = get_node_id();
        msg.state = (uint8_t)get_state();

        esp_err_t err = esp_now_send(
            peer_mac,
            (uint8_t *)&msg,
            sizeof(msg)
        );

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send failed: %s",
                     esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(
            CONFIG_REDUNDANCY_HEARTBEAT_INTERVAL_MS));
    }
}


/* ---------------------------------------------------------
 * State manager task
 * --------------------------------------------------------- */
static void state_manager_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "State manager started");

    while (1) {
        TickType_t now = xTaskGetTickCount();
        TickType_t peer_last_seen;
        node_state_t local_state;
        node_state_t remote_state;
        bool seen;

        taskENTER_CRITICAL(&redundancy_mux);
        peer_last_seen = last_peer_heartbeat;
        local_state = current_state;
        remote_state = peer_state;
        seen = peer_seen;
        taskEXIT_CRITICAL(&redundancy_mux);

        bool startup_complete =
            (now - startup_time) >= pdMS_TO_TICKS(STARTUP_GRACE_MS);

        bool peer_alive =
            seen &&
            ((now - peer_last_seen) < pdMS_TO_TICKS(PEER_TIMEOUT_MS));

        if (local_state == NODE_INIT) {
            if (startup_complete) {
                if (!peer_alive) {
                    ESP_LOGW(TAG,
                             "No peer heartbeat during startup; "
                             "becoming ACTIVE");
                    set_state(NODE_ACTIVE);
                } else if (remote_state == NODE_ACTIVE) {
                    set_state(NODE_REST);
                } else {
                    /*
                     * Initial election: both nodes are starting
                     * together or the peer has not become ACTIVE.
                     * The lower node ID wins this initial election.
                     */
                    if (get_node_id() < (uint8_t)CONFIG_REDUNDANCY_PEER_NODE_ID[0]) {
                        set_state(NODE_ACTIVE);
                    } else {
                        set_state(NODE_REST);
                    }
                }
            }
        } else if (local_state == NODE_ACTIVE) {
            /*
             * If both nodes report ACTIVE, use the node ID
             * as a tie-breaker to converge on one active node.
             */
            if (peer_alive && remote_state == NODE_ACTIVE) {
                if (get_node_id() >
                    (uint8_t)CONFIG_REDUNDANCY_PEER_NODE_ID[0]) {
                    set_state(NODE_REST);
                }
            }
        } else if (local_state == NODE_REST) {
            if (!peer_alive) {
                ESP_LOGW(TAG, "Peer heartbeat timeout");
                set_state(NODE_ACTIVE);
            } else if (remote_state == NODE_ACTIVE) {
                /* Stay REST while the peer is active. */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(STATE_TASK_INTERVAL_MS));
    }
}


/* ---------------------------------------------------------
 * Redundancy initialization
 * --------------------------------------------------------- */
esp_err_t redundancy_init(void)
{
    uint8_t local_mac[ESP_NOW_ETH_ALEN];

    ESP_LOGI(TAG, "Initializing redundancy system");
    ESP_LOGI(TAG, "Configured Node ID: %s",
             CONFIG_REDUNDANCY_NODE_ID);
    ESP_LOGI(TAG, "Configured Peer MAC: %s",
             CONFIG_REDUNDANCY_PEER_MAC);

    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, local_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get Wi-Fi MAC: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Local node MAC:");
    print_mac(local_mac);

    err = parse_peer_mac(CONFIG_REDUNDANCY_PEER_MAC, peer_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse peer MAC");
        return err;
    }

    ESP_LOGI(TAG, "Parsed peer MAC:");
    print_mac(peer_mac);

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_send_cb(send_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send callback registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_recv_cb(recv_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Receive callback registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    esp_now_peer_info_t peer = {0};

    memcpy(peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ESP-NOW peer: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW peer configured:");
    print_mac(peer_mac);

    startup_time = xTaskGetTickCount();

    BaseType_t result = xTaskCreate(
        heartbeat_task,
        "heartbeat_task",
        4096,
        NULL,
        3,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
        return ESP_ERR_NO_MEM;
    }

    result = xTaskCreate(
        state_manager_task,
        "state_manager",
        4096,
        NULL,
        4,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create state manager task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Redundancy state machine started");
    return ESP_OK;
}