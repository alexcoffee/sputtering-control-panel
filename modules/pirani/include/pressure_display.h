#ifndef PRESSURE_DISPLAY_H
#define PRESSURE_DISPLAY_H

#include <stdint.h>

#include "pressure_format.h"
#include "pressure_display_spi.h"

void pressure_display_init(const pressure_display_spi_pins_t *pins);
void pressure_display_tick(uint32_t elapsed_ms);
void pressure_display_task_handler(void);
void pressure_display_render(float torr_value, float voltage, pressure_display_unit_t unit);
void pressure_display_render_unplugged(void);

#endif /* PRESSURE_DISPLAY_H */
