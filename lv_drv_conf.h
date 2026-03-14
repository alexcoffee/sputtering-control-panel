#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#include "lv_conf.h"

#define LV_DRV_DELAY_INCLUDE <pico/stdlib.h>
#define LV_DRV_DELAY_US(us) sleep_us(us)
#define LV_DRV_DELAY_MS(ms) sleep_ms(ms)

#define LV_DRV_DISP_INCLUDE <pressure_display_spi.h>
#define LV_DRV_DISP_CMD_DATA(val) pressure_display_spi_set_cd((val))
#define LV_DRV_DISP_RST(val) pressure_display_spi_set_reset((val))
#define LV_DRV_DISP_SPI_CS(val) pressure_display_spi_set_cs((val))
#define LV_DRV_DISP_SPI_WR_BYTE(data) pressure_display_spi_write_byte((data))
#define LV_DRV_DISP_SPI_WR_ARRAY(adr, n) pressure_display_spi_write_array((const uint8_t *)(adr), (n))

#define USE_GC9A01 1

#endif /* LV_DRV_CONF_H */
