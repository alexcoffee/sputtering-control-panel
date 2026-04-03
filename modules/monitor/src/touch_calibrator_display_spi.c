#include "touch_calibrator_display_spi.h"

#include <stdio.h>

#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define LCD_SPI_BAUD_HZ (10U * 1000U * 1000U)
/* ADS7843/XPT2046-compatible controllers are happier with modest SPI clocks. */
#define TOUCH_SPI_BAUD_HZ (500U * 1000U)
#define XPT2046_CMD_READ_X 0x90
#define XPT2046_CMD_READ_Y 0xD0
#define XPT2046_SAMPLE_COUNT 7

static touch_calibrator_display_spi_pins_t s_pins;
static spi_inst_t *s_lcd_spi;
static spi_inst_t *s_touch_spi;
static uint32_t s_touch_zero_read_count;

static spi_inst_t *spi_from_index(uint8_t index) {
    if (index == 0U) {
        return spi0;
    }
    if (index == 1U) {
        return spi1;
    }
    return NULL;
}

static void sort_u16(uint16_t *values, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        const uint16_t key = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

static uint8_t spi_xchg(uint8_t tx_byte) {
    uint8_t rx_byte = 0U;
    (void)spi_write_read_blocking(s_touch_spi, &tx_byte, &rx_byte, 1U);
    return rx_byte;
}

static void xpt2046_read_xy(uint16_t *out_x, uint16_t *out_y) {
    uint16_t x = 0U;
    uint16_t y = 0U;

    (void)spi_xchg(XPT2046_CMD_READ_X);
    x = (uint16_t)spi_xchg(0x00U) << 8;
    x |= (uint16_t)spi_xchg(XPT2046_CMD_READ_Y);

    y = (uint16_t)spi_xchg(0x00U) << 8;
    y |= (uint16_t)spi_xchg(0x00U);

    if (out_x != NULL) {
        *out_x = (uint16_t)(x >> 3);
    }
    if (out_y != NULL) {
        *out_y = (uint16_t)(y >> 3);
    }
}

void touch_calibrator_display_spi_init(const touch_calibrator_display_spi_pins_t *pins) {
    if (pins == NULL) {
        return;
    }

    s_pins = *pins;
    s_lcd_spi = spi_from_index(s_pins.lcd_spi_index);
    s_touch_spi = spi_from_index(s_pins.touch_spi_index);
    s_touch_zero_read_count = 0U;

    if (s_lcd_spi == NULL || s_touch_spi == NULL || s_lcd_spi == s_touch_spi) {
        printf("touch_calibrator_display_spi_init: invalid SPI selection (lcd=%u touch=%u)\n",
               (unsigned int)s_pins.lcd_spi_index,
               (unsigned int)s_pins.touch_spi_index);
        return;
    }

    spi_init(s_lcd_spi, LCD_SPI_BAUD_HZ);
    spi_set_format(s_lcd_spi, 8U, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(s_pins.lcd_spi_tx_pin, GPIO_FUNC_SPI);
    gpio_set_function(s_pins.lcd_spi_rx_pin, GPIO_FUNC_SPI);
    gpio_set_function(s_pins.lcd_spi_sck_pin, GPIO_FUNC_SPI);

    spi_init(s_touch_spi, TOUCH_SPI_BAUD_HZ);
    spi_set_format(s_touch_spi, 8U, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(s_pins.touch_spi_tx_pin, GPIO_FUNC_SPI);
    gpio_set_function(s_pins.touch_spi_rx_pin, GPIO_FUNC_SPI);
    gpio_set_function(s_pins.touch_spi_sck_pin, GPIO_FUNC_SPI);

    gpio_init(s_pins.lcd_spi_csn_pin);
    gpio_set_dir(s_pins.lcd_spi_csn_pin, GPIO_OUT);
    gpio_put(s_pins.lcd_spi_csn_pin, 1);

    gpio_init(s_pins.lcd_reset_pin);
    gpio_set_dir(s_pins.lcd_reset_pin, GPIO_OUT);
    gpio_put(s_pins.lcd_reset_pin, 1);

    gpio_init(s_pins.lcd_command_pin);
    gpio_set_dir(s_pins.lcd_command_pin, GPIO_OUT);
    gpio_put(s_pins.lcd_command_pin, 1);

    gpio_set_function(s_pins.lcd_backlight_pin, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(s_pins.lcd_backlight_pin);
    const uint chan = pwm_gpio_to_channel(s_pins.lcd_backlight_pin);
    pwm_set_wrap(slice, 255U);
    pwm_set_chan_level(slice, chan, 255U);
    pwm_set_enabled(slice, true);

    gpio_init(s_pins.touch_spi_csn_pin);
    gpio_set_dir(s_pins.touch_spi_csn_pin, GPIO_OUT);
    gpio_put(s_pins.touch_spi_csn_pin, 1);

    gpio_init(s_pins.touch_irq_pin);
    gpio_set_dir(s_pins.touch_irq_pin, GPIO_IN);
    gpio_pull_up(s_pins.touch_irq_pin);
}

void touch_calibrator_display_spi_set_lcd_cd(bool value) {
    gpio_put(s_pins.lcd_command_pin, value);
}

void touch_calibrator_display_spi_set_lcd_reset(bool value) {
    gpio_put(s_pins.lcd_reset_pin, value);
}

void touch_calibrator_display_spi_set_lcd_cs(bool value) {
    gpio_put(s_pins.lcd_spi_csn_pin, value);
}

void touch_calibrator_display_spi_write_byte(uint8_t byte) {
    (void)spi_write_blocking(s_lcd_spi, &byte, 1U);
}

void touch_calibrator_display_spi_write_array(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0U) {
        return;
    }

    (void)spi_write_blocking(s_lcd_spi, data, len);
}

bool touch_calibrator_display_spi_touch_irq_active(void) {
    return gpio_get(s_pins.touch_irq_pin) == 0U;
}

bool touch_calibrator_display_spi_touch_read_raw(uint16_t *out_x, uint16_t *out_y) {
    if (out_x == NULL || out_y == NULL || !touch_calibrator_display_spi_touch_irq_active()) {
        return false;
    }

    uint16_t x_samples[XPT2046_SAMPLE_COUNT];
    uint16_t y_samples[XPT2046_SAMPLE_COUNT];

    touch_calibrator_display_spi_set_lcd_cs(true);
    spi_set_baudrate(s_touch_spi, TOUCH_SPI_BAUD_HZ);
    spi_set_format(s_touch_spi, 8U, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(s_pins.touch_spi_csn_pin, 0);
    sleep_us(2);

    /* Throw away first sample after CS assert; XPT2046 often needs one settle read. */
    xpt2046_read_xy(NULL, NULL);
    for (size_t i = 0; i < XPT2046_SAMPLE_COUNT; ++i) {
        xpt2046_read_xy(&x_samples[i], &y_samples[i]);
    }

    sleep_us(2);
    gpio_put(s_pins.touch_spi_csn_pin, 1);

    if (!touch_calibrator_display_spi_touch_irq_active()) {
        return false;
    }

    sort_u16(x_samples, XPT2046_SAMPLE_COUNT);
    sort_u16(y_samples, XPT2046_SAMPLE_COUNT);

    *out_x = x_samples[XPT2046_SAMPLE_COUNT / 2U];
    *out_y = y_samples[XPT2046_SAMPLE_COUNT / 2U];

    if (*out_x == 0U && *out_y == 0U) {
        s_touch_zero_read_count++;
        if ((s_touch_zero_read_count % 100U) == 0U) {
            printf("touch debug: still reading (0,0) while IRQ active; check touch MISO path\n");
        }
    } else {
        s_touch_zero_read_count = 0U;
    }

    return true;
}
