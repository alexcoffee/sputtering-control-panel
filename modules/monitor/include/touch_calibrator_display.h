#ifndef TOUCH_CALIBRATOR_DISPLAY_H
#define TOUCH_CALIBRATOR_DISPLAY_H

#include <stdint.h>

#include "touch_calibrator_display_spi.h"

void touch_calibrator_display_init(const touch_calibrator_display_spi_pins_t *pins);
void touch_calibrator_display_tick(uint32_t elapsed_ms);
void touch_calibrator_display_task_handler(void);

#endif /* TOUCH_CALIBRATOR_DISPLAY_H */
