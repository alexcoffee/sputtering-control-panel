#ifndef DATA_LOGGER_DISPLAY_H
#define DATA_LOGGER_DISPLAY_H

#include <stdint.h>

#include "data_logger_display_spi.h"

struct can2040_msg;

void data_logger_display_init(const data_logger_display_spi_pins_t *pins);
void data_logger_display_tick(uint32_t elapsed_ms);
void data_logger_display_task_handler(void);
void data_logger_display_append_can_event(const struct can2040_msg *msg, uint32_t uptime_ms);

#endif /* DATA_LOGGER_DISPLAY_H */
