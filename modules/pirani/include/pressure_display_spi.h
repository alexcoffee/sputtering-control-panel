#ifndef PRESSURE_DISPLAY_SPI_H
#define PRESSURE_DISPLAY_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t spi_sck_pin;
    uint8_t spi_tx_pin;
    uint8_t spi_csn_pin;
    uint8_t command_pin;
    uint8_t reset_pin;
    uint8_t backlight_pin;
} pressure_display_spi_pins_t;

void pressure_display_spi_init(const pressure_display_spi_pins_t *pins);
void pressure_display_spi_set_cd(bool value);
void pressure_display_spi_set_reset(bool value);
void pressure_display_spi_set_cs(bool value);
void pressure_display_spi_write_byte(uint8_t byte);
void pressure_display_spi_write_array(const uint8_t *data, size_t len);

#endif /* PRESSURE_DISPLAY_SPI_H */
