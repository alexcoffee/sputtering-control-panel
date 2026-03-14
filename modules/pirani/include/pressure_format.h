#ifndef PRESSURE_FORMAT_H
#define PRESSURE_FORMAT_H

typedef enum {
    PRESSURE_DISPLAY_UNIT_TORR = 0,
    PRESSURE_DISPLAY_UNIT_BAR = 1,
    PRESSURE_DISPLAY_UNIT_VOLTAGE = 2,
} pressure_display_unit_t;

void pressure_format_reading(char *value_buffer,
                             int value_buffer_len,
                             char *unit_buffer,
                             int unit_buffer_len,
                             float torr_value,
                             float voltage,
                             pressure_display_unit_t unit);

#endif /* PRESSURE_FORMAT_H */
