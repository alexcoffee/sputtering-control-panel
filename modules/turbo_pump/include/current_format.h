#ifndef CURRENT_FORMAT_H
#define CURRENT_FORMAT_H

typedef enum {
    CURRENT_DISPLAY_UNIT_AMP = 0,
    CURRENT_DISPLAY_UNIT_VOLTAGE = 1,
} current_display_unit_t;

void current_format_reading(char *value_buffer,
                            int value_buffer_len,
                            char *unit_buffer,
                            int unit_buffer_len,
                            float current_amps,
                            float voltage,
                            current_display_unit_t unit);

#endif /* CURRENT_FORMAT_H */
