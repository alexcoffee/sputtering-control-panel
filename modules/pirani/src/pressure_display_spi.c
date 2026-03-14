#include "pressure_display_spi.h"

#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

static pressure_display_spi_pins_t s_pins;

void pressure_display_spi_init(const pressure_display_spi_pins_t *pins) {
    if (pins == NULL) {
        return;
    }

    s_pins = *pins;

    spi_init(spi_default, 100U * 1000U * 1000U);
    gpio_set_function(s_pins.spi_tx_pin, GPIO_FUNC_SPI);
    gpio_set_function(s_pins.spi_sck_pin, GPIO_FUNC_SPI);

    gpio_init(s_pins.spi_csn_pin);
    gpio_set_dir(s_pins.spi_csn_pin, GPIO_OUT);
    gpio_put(s_pins.spi_csn_pin, 1);

    gpio_init(s_pins.reset_pin);
    gpio_set_dir(s_pins.reset_pin, GPIO_OUT);
    gpio_put(s_pins.reset_pin, 1);

    gpio_init(s_pins.command_pin);
    gpio_set_dir(s_pins.command_pin, GPIO_OUT);
    gpio_put(s_pins.command_pin, 1);

    gpio_set_function(s_pins.backlight_pin, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(s_pins.backlight_pin);
    const uint chan = pwm_gpio_to_channel(s_pins.backlight_pin);
    pwm_set_wrap(slice, 255U);
    pwm_set_chan_level(slice, chan, 255U);
    pwm_set_enabled(slice, true);
}

void pressure_display_spi_set_cd(bool value) {
    gpio_put(s_pins.command_pin, value);
}

void pressure_display_spi_set_reset(bool value) {
    gpio_put(s_pins.reset_pin, value);
}

void pressure_display_spi_set_cs(bool value) {
    gpio_put(s_pins.spi_csn_pin, value);
}

void pressure_display_spi_write_byte(uint8_t byte) {
    (void)spi_write_blocking(spi_default, &byte, 1U);
}

void pressure_display_spi_write_array(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0U) {
        return;
    }
    (void)spi_write_blocking(spi_default, data, (size_t)len);
}
