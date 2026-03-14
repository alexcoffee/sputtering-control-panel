#include "pressure_format.h"

#include <stdio.h>

static void pressure_format_torr(char *value_buffer,
                                 int value_buffer_len,
                                 char *unit_buffer,
                                 int unit_buffer_len,
                                 float torr_value) {
    if (torr_value >= 1.0f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", torr_value);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "Torr");
    } else if (torr_value >= 1e-3f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", torr_value * 1e3f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "mTorr");
    } else if (torr_value >= 1e-6f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", torr_value * 1e6f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "uTorr");
    } else if (torr_value >= 1e-9f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", torr_value * 1e9f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "nTorr");
    } else {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", torr_value * 1e12f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "pTorr");
    }
}

static void pressure_format_bar(char *value_buffer,
                                int value_buffer_len,
                                char *unit_buffer,
                                int unit_buffer_len,
                                float torr_value) {
    const float bar_value = torr_value * 1.333223684f * 1e-3f;

    if (bar_value >= 1.0f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.3f", bar_value);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "bar");
    } else if (bar_value >= 1e-3f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", bar_value * 1e3f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "mbar");
    } else if (bar_value >= 1e-6f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", bar_value * 1e6f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "ubar");
    } else if (bar_value >= 1e-9f) {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", bar_value * 1e9f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "nbar");
    } else {
        (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.0f", bar_value * 1e12f);
        (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "pbar");
    }
}

static void pressure_format_voltage(char *value_buffer,
                                    int value_buffer_len,
                                    char *unit_buffer,
                                    int unit_buffer_len,
                                    float voltage) {
    (void)snprintf(value_buffer, (size_t)value_buffer_len, "%.3f", voltage);
    (void)snprintf(unit_buffer, (size_t)unit_buffer_len, "V");
}

void pressure_format_reading(char *value_buffer,
                             int value_buffer_len,
                             char *unit_buffer,
                             int unit_buffer_len,
                             float torr_value,
                             float voltage,
                             pressure_display_unit_t unit) {
    switch (unit) {
        case PRESSURE_DISPLAY_UNIT_BAR:
            pressure_format_bar(value_buffer, value_buffer_len, unit_buffer, unit_buffer_len, torr_value);
            break;
        case PRESSURE_DISPLAY_UNIT_VOLTAGE:
            pressure_format_voltage(value_buffer, value_buffer_len, unit_buffer, unit_buffer_len, voltage);
            break;
        case PRESSURE_DISPLAY_UNIT_TORR:
        default:
            pressure_format_torr(value_buffer, value_buffer_len, unit_buffer, unit_buffer_len, torr_value);
            break;
    }
}
