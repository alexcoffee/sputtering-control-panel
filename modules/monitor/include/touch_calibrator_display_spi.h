#ifndef TOUCH_CALIBRATOR_DISPLAY_SPI_H
#define TOUCH_CALIBRATOR_DISPLAY_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t lcd_spi_index;
    uint8_t lcd_spi_sck_pin;
    uint8_t lcd_spi_tx_pin;
    uint8_t lcd_spi_rx_pin;
    uint8_t lcd_spi_csn_pin;
    uint8_t lcd_command_pin;
    uint8_t lcd_reset_pin;
    uint8_t lcd_backlight_pin;
    uint8_t touch_spi_index;
    uint8_t touch_spi_sck_pin;
    uint8_t touch_spi_tx_pin;
    uint8_t touch_spi_rx_pin;
    uint8_t touch_spi_csn_pin;
    uint8_t touch_irq_pin;
    uint8_t encoder_a_pin;
    uint8_t encoder_b_pin;
    uint8_t encoder_button_pin;
} touch_calibrator_display_spi_pins_t;

void touch_calibrator_display_spi_init(const touch_calibrator_display_spi_pins_t *pins);
void touch_calibrator_display_spi_set_lcd_cd(bool value);
void touch_calibrator_display_spi_set_lcd_reset(bool value);
void touch_calibrator_display_spi_set_lcd_cs(bool value);
void touch_calibrator_display_spi_write_byte(uint8_t byte);
void touch_calibrator_display_spi_write_array(const uint8_t *data, size_t len);
bool touch_calibrator_display_spi_touch_irq_active(void);
bool touch_calibrator_display_spi_touch_read_raw(uint16_t *out_x, uint16_t *out_y);
void touch_calibrator_display_spi_set_backlight_percent(uint8_t percent);
uint8_t touch_calibrator_display_spi_get_backlight_percent(void);

#endif /* TOUCH_CALIBRATOR_DISPLAY_SPI_H */
