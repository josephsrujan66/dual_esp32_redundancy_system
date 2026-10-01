#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include <stdint.h>

#define HEARTBEAT_INTERVAL_MS       250
#define PEER_TIMEOUT_MS              1000
#define ELECTION_WINDOW_MS           2000

#define SENSOR_SAMPLE_INTERVAL_MS    250
#define MQTT_PUBLISH_INTERVAL_MS     1000

#define MQTT_TOPIC                   "dual-esp32/environment"

#define DEVICE_NAME_A                "NODE_A"
#define DEVICE_NAME_B                "NODE_B"

typedef enum {
    ROLE_DISCOVERY = 0,
    ROLE_ACTIVE,
    ROLE_REST,
    ROLE_TAKEOVER
} node_role_t;

typedef struct {
    float temperature;
    float humidity;
    bool valid;
} sensor_data_t;

#endif