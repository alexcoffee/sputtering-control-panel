#ifndef CURRENT_SENSOR_H
#define CURRENT_SENSOR_H

#include <stdint.h>

typedef struct {
    float voltage;
    float current_amps;
} current_sensor_reading_t;

void current_sensor_init(uint8_t adc_gpio);
current_sensor_reading_t current_sensor_read_amps(void);

#endif /* CURRENT_SENSOR_H */
