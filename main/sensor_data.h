#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t temperature;
    uint8_t humidity;
    bool valid;
    uint32_t sequence;
} sensor_data_t;

void sensor_data_init(void);

void sensor_data_write(uint8_t temperature,
                       uint8_t humidity);

bool sensor_data_read(sensor_data_t *data);

#endif