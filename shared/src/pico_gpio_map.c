#include "scp/pico_gpio_map.h"

#include <stdio.h>
#include <string.h>

static void scp_set_error(char *error_buf, size_t error_buf_size, const char *message) {
    if (error_buf == NULL || error_buf_size == 0) {
        return;
    }

    (void)snprintf(error_buf, error_buf_size, "%s", message);
}

bool scp_pico_gpio_map_build(const scp_gpio_assignment_t *assignments,
                             size_t assignment_count,
                             scp_pico_gpio_map_t *out_map,
                             char *error_buf,
                             size_t error_buf_size) {
    size_t i;

    if (out_map == NULL) {
        scp_set_error(error_buf, error_buf_size, "out_map is NULL");
        return false;
    }

    for (i = 0; i < SCP_PICO_GPIO_COUNT; ++i) {
        out_map->signal_by_gpio[i] = NULL;
    }

    if (assignments == NULL || assignment_count == 0) {
        scp_set_error(error_buf, error_buf_size, "no GPIO assignments provided");
        return false;
    }

    for (i = 0; i < assignment_count; ++i) {
        const scp_gpio_assignment_t *assignment = &assignments[i];
        const char *existing_signal;

        if (assignment->signal_name == NULL) {
            scp_set_error(error_buf, error_buf_size, "assignment has NULL signal name");
            return false;
        }

        if (assignment->gpio < SCP_PICO_GPIO_MIN || assignment->gpio > SCP_PICO_GPIO_MAX) {
            (void)snprintf(error_buf,
                           error_buf_size,
                           "signal %s assigned to invalid GPIO %u",
                           assignment->signal_name,
                           assignment->gpio);
            return false;
        }

        existing_signal = out_map->signal_by_gpio[assignment->gpio];
        if (existing_signal != NULL) {
            (void)snprintf(error_buf,
                           error_buf_size,
                           "GPIO %u collision: %s and %s",
                           assignment->gpio,
                           existing_signal,
                           assignment->signal_name);
            return false;
        }

        out_map->signal_by_gpio[assignment->gpio] = assignment->signal_name;
    }

    return true;
}

bool scp_pico_gpio_map_find_pin(const scp_pico_gpio_map_t *map, const char *signal_name, uint8_t *out_gpio) {
    size_t gpio;

    if (map == NULL || signal_name == NULL || out_gpio == NULL) {
        return false;
    }

    for (gpio = 0; gpio < SCP_PICO_GPIO_COUNT; ++gpio) {
        if (map->signal_by_gpio[gpio] != NULL && strcmp(map->signal_by_gpio[gpio], signal_name) == 0) {
            *out_gpio = (uint8_t)gpio;
            return true;
        }
    }

    return false;
}
