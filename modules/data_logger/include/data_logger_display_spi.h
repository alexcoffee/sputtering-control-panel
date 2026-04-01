#ifndef DATA_LOGGER_DISPLAY_SPI_H
#define DATA_LOGGER_DISPLAY_SPI_H

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
} data_logger_display_spi_pins_t;

void data_logger_display_spi_init(const data_logger_display_spi_pins_t *pins);
void data_logger_display_spi_set_cd(bool value);
void data_logger_display_spi_set_reset(bool value);
void data_logger_display_spi_set_cs(bool value);
void data_logger_display_spi_write_byte(uint8_t byte);
void data_logger_display_spi_write_array(const uint8_t *data, size_t len);

#endif /* DATA_LOGGER_DISPLAY_SPI_H */
