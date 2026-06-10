#include "current_format.h"

#include <stdio.h>

static void current_format_watts(char *value_buffer,
                                 int value_buffer_len,
                                 char *unit_buffer,
                                 int unit_buffer_len,
                                 float current_amps) {
    const float watts = current_amps * TURBO_PUMP_BUS_VOLTAGE;
    (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", watts);
    (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "W");
}

static void current_format_voltage(char *value_buffer,
                                   int value_buffer_len,
                                   char *unit_buffer,
                                   int unit_buffer_len,
                                   float voltage) {
    (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.3f", voltage);
    (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "V");
}

void current_format_reading(char *value_buffer,
                            int value_buffer_len,
                            char *unit_buffer,
                            int unit_buffer_len,
                            float current_amps,
                            float voltage,
                            current_display_unit_t unit) {
    switch (unit) {
        case CURRENT_DISPLAY_UNIT_VOLTAGE:
            current_format_voltage(value_buffer, value_buffer_len, unit_buffer, unit_buffer_len, voltage);
            break;
        case CURRENT_DISPLAY_UNIT_WATT:
        default:
            current_format_watts(value_buffer, value_buffer_len, unit_buffer, unit_buffer_len, current_amps);
            break;
    }
}
