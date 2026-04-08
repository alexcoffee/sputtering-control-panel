#include <stdio.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "module_config.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/module_ids.h"
#include "touch_calibrator_display.h"

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SCP_GPIO_SIGNAL_HEARTBEAT_LED, 25},

    {SCP_GPIO_SIGNAL_CAN_TX, 0},
    {SCP_GPIO_SIGNAL_CAN_RX, 1},

    {SCP_GPIO_SIGNAL_LCD_SPI_CSN, 2},
    {SCP_GPIO_SIGNAL_LCD_RESET, 3},
    {SCP_GPIO_SIGNAL_LCD_SDO, 4},
    {SCP_GPIO_SIGNAL_LCD_COMMAND, 5},
    {SCP_GPIO_SIGNAL_LCD_SPI_SCK, 6},
    {SCP_GPIO_SIGNAL_LCD_SDI, 7},
    {SCP_GPIO_SIGNAL_LCD_BACKLIGHT, 8},
    {SCP_GPIO_SIGNAL_TOUCH_SPI_CSN, 9},
    {SCP_GPIO_SIGNAL_TOUCH_SPI_SCK, 10},
    {SCP_GPIO_SIGNAL_TOUCH_SDI, 11},
    {SCP_GPIO_SIGNAL_TOUCH_SDO, 12},
    {SCP_GPIO_SIGNAL_TOUCH_IRQ, 13},
    {SCP_GPIO_SIGNAL_SWITCH_A, 14},
    {SCP_GPIO_SIGNAL_SWITCH_B, 15},
    {SCP_GPIO_SIGNAL_SWITCH_ENABLE, 16},
};

const scp_module_config_t g_module_config = {
    .module_name = "monitor",
    .module_id = SCP_MODULE_ID_MONITOR,
    .can_pio_num = 0,
    .can_bitrate = 500000,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

static bool gpio_in_list(uint8_t gpio, const uint8_t *list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == gpio) {
            return true;
        }
    }
    return false;
}

static bool try_get_spi_index_for_pin_triplet(uint8_t sck_pin, uint8_t tx_pin, uint8_t rx_pin, uint8_t *out_spi_index) {
    static const uint8_t spi0_sck_pins[] = {2U, 6U, 18U, 22U};
    static const uint8_t spi0_tx_pins[] = {3U, 7U, 19U, 23U};
    static const uint8_t spi0_rx_pins[] = {0U, 4U, 16U, 20U};

    static const uint8_t spi1_sck_pins[] = {10U, 14U, 26U};
    static const uint8_t spi1_tx_pins[] = {11U, 15U, 27U};
    static const uint8_t spi1_rx_pins[] = {8U, 12U, 24U, 28U};

    if (gpio_in_list(sck_pin, spi0_sck_pins, sizeof(spi0_sck_pins) / sizeof(spi0_sck_pins[0]))
        && gpio_in_list(tx_pin, spi0_tx_pins, sizeof(spi0_tx_pins) / sizeof(spi0_tx_pins[0]))
        && gpio_in_list(rx_pin, spi0_rx_pins, sizeof(spi0_rx_pins) / sizeof(spi0_rx_pins[0]))) {
        if (out_spi_index != NULL) {
            *out_spi_index = 0U;
        }
        return true;
    }

    if (gpio_in_list(sck_pin, spi1_sck_pins, sizeof(spi1_sck_pins) / sizeof(spi1_sck_pins[0]))
        && gpio_in_list(tx_pin, spi1_tx_pins, sizeof(spi1_tx_pins) / sizeof(spi1_tx_pins[0]))
        && gpio_in_list(rx_pin, spi1_rx_pins, sizeof(spi1_rx_pins) / sizeof(spi1_rx_pins[0]))) {
        if (out_spi_index != NULL) {
            *out_spi_index = 1U;
        }
        return true;
    }

    return false;
}

int main(void) {
    stdio_init_all();

    scp_can_bus_t can_bus;
    scp_pico_gpio_map_t gpio_map;
    struct can2040_msg tx_msg;
    struct can2040_msg rx_msg;

    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t heartbeat_led_gpio;
    uint8_t lcd_spi_sck_gpio;
    uint8_t lcd_spi_tx_gpio;
    uint8_t lcd_spi_rx_gpio;
    uint8_t lcd_spi_csn_gpio;
    uint8_t lcd_command_gpio;
    uint8_t lcd_reset_gpio;
    uint8_t lcd_backlight_gpio;
    uint8_t touch_spi_sck_gpio;
    uint8_t touch_spi_tx_gpio;
    uint8_t touch_spi_rx_gpio;
    uint8_t touch_spi_csn_gpio;
    uint8_t touch_irq_gpio;
    uint8_t encoder_a_gpio;
    uint8_t encoder_b_gpio;
    uint8_t encoder_button_gpio;
    uint8_t lcd_spi_index;
    uint8_t touch_spi_index;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SDI, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SDO, &lcd_spi_rx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_TOUCH_SPI_SCK, &touch_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_TOUCH_SDI, &touch_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_TOUCH_SDO, &touch_spi_rx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_TOUCH_SPI_CSN, &touch_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_TOUCH_IRQ, &touch_irq_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_A, &encoder_a_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_B, &encoder_b_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_ENABLE, &encoder_button_gpio)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
        return 1;
    }

    if (!try_get_spi_index_for_pin_triplet(lcd_spi_sck_gpio, lcd_spi_tx_gpio, lcd_spi_rx_gpio, &lcd_spi_index)) {
        printf("%s pin map error: invalid LCD SPI pins (SCK=%u TX=%u RX=%u)\n",
               g_module_config.module_name,
               lcd_spi_sck_gpio,
               lcd_spi_tx_gpio,
               lcd_spi_rx_gpio);
        return 1;
    }

    if (!try_get_spi_index_for_pin_triplet(touch_spi_sck_gpio, touch_spi_tx_gpio, touch_spi_rx_gpio, &touch_spi_index)) {
        printf("%s pin map error: invalid touch SPI pins (SCK=%u TX=%u RX=%u)\n",
               g_module_config.module_name,
               touch_spi_sck_gpio,
               touch_spi_tx_gpio,
               touch_spi_rx_gpio);
        return 1;
    }

    if (lcd_spi_index == touch_spi_index) {
        printf("%s pin map error: LCD and touch must use different SPI blocks (both SPI%u)\n",
               g_module_config.module_name,
               (unsigned int)lcd_spi_index);
        return 1;
    }

    gpio_init(heartbeat_led_gpio);
    gpio_set_dir(heartbeat_led_gpio, GPIO_OUT);
    gpio_put(heartbeat_led_gpio, 0);

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }

    const touch_calibrator_display_spi_pins_t lcd_pins = {
        .lcd_spi_index = lcd_spi_index,
        .lcd_spi_sck_pin = lcd_spi_sck_gpio,
        .lcd_spi_tx_pin = lcd_spi_tx_gpio,
        .lcd_spi_rx_pin = lcd_spi_rx_gpio,
        .lcd_spi_csn_pin = lcd_spi_csn_gpio,
        .lcd_command_pin = lcd_command_gpio,
        .lcd_reset_pin = lcd_reset_gpio,
        .lcd_backlight_pin = lcd_backlight_gpio,
        .touch_spi_index = touch_spi_index,
        .touch_spi_sck_pin = touch_spi_sck_gpio,
        .touch_spi_tx_pin = touch_spi_tx_gpio,
        .touch_spi_rx_pin = touch_spi_rx_gpio,
        .touch_spi_csn_pin = touch_spi_csn_gpio,
        .touch_irq_pin = touch_irq_gpio,
        .encoder_a_pin = encoder_a_gpio,
        .encoder_b_pin = encoder_b_gpio,
        .encoder_button_pin = encoder_button_gpio,
    };
    touch_calibrator_display_init(&lcd_pins);

    printf("%s online (module_id=%u)\n", g_module_config.module_name, g_module_config.module_id);

    absolute_time_t next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    uint8_t heartbeat_counter = 0;
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;

        if (elapsed_lvgl_ms != 0U) {
            touch_calibrator_display_tick(elapsed_lvgl_ms);
            last_lvgl_tick_ms = now_ms;
        }
        touch_calibrator_display_task_handler();

        if (scp_can_try_read(&can_bus, &rx_msg)) {
            touch_calibrator_display_handle_can_message(&rx_msg, now_ms);
        }

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
            (void)scp_can_transmit(&can_bus, &tx_msg);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);

            gpio_put(heartbeat_led_gpio, 1);
            sleep_us(SCP_LED_FLASH_PULSE_US);
            gpio_put(heartbeat_led_gpio, 0);
        }

        sleep_us(100);
    }
}
