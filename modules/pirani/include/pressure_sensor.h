#ifndef PRESSURE_SENSOR_H
#define PRESSURE_SENSOR_H

#include <stdint.h>

#define PRESSURE_SENSOR_MIN_VALID_VOLTAGE 1.9f

typedef struct {
    float voltage;
    float pressure_torr;
} pressure_sensor_reading_t;

void pressure_sensor_init(uint8_t adc_gpio);
pressure_sensor_reading_t pressure_sensor_read_torr(void);

#endif /* PRESSURE_SENSOR_H */
