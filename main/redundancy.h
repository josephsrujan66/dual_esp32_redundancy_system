#ifndef REDUNDANCY_H
#define REDUNDANCY_H

#include "esp_err.h"
#include <stdbool.h>
esp_err_t redundancy_init(void);
bool redundancy_is_active(void);

#endif