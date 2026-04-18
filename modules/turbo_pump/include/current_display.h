#ifndef CURRENT_DISPLAY_H
#define CURRENT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "current_format.h"
#include "pressure_display_spi.h"

void current_display_init(const pressure_display_spi_pins_t *pins);
void current_display_tick(uint32_t elapsed_ms);
void current_display_task_handler(void);
void current_display_set_turbo_state(bool enabled, bool low_speed_enabled);
void current_display_render(float current_amps, float voltage, current_display_unit_t unit);
void current_display_render_unplugged(void);

#endif /* CURRENT_DISPLAY_H */
