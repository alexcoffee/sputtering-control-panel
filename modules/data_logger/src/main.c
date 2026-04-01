#include <stdio.h>

#include "pico/stdlib.h"

#include "data_logger_display.h"
#include "module_config.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SCP_GPIO_SIGNAL_HEARTBEAT_LED, 25},

    {SCP_GPIO_SIGNAL_CAN_RX, 1},
    {SCP_GPIO_SIGNAL_CAN_TX, 0},

    {SCP_GPIO_SIGNAL_LCD_SPI_SCK, 2},
    {SCP_GPIO_SIGNAL_LCD_SPI_TX, 3},
    {SCP_GPIO_SIGNAL_LCD_COMMAND, 4},
    {SCP_GPIO_SIGNAL_LCD_SPI_CSN, 5},
    {SCP_GPIO_SIGNAL_LCD_RESET, 6},
    {SCP_GPIO_SIGNAL_LCD_BACKLIGHT, 8},
};

const scp_module_config_t g_module_config = {
    .module_name = "data_logger",
    .module_id = 10,
    .can_pio_num = 0,
    .can_bitrate = 500000,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

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
    uint8_t lcd_spi_csn_gpio;
    uint8_t lcd_command_gpio;
    uint8_t lcd_reset_gpio;
    uint8_t lcd_backlight_gpio;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_TX, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
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

    const data_logger_display_spi_pins_t lcd_pins = {
        .spi_sck_pin = lcd_spi_sck_gpio,
        .spi_tx_pin = lcd_spi_tx_gpio,
        .spi_csn_pin = lcd_spi_csn_gpio,
        .command_pin = lcd_command_gpio,
        .reset_pin = lcd_reset_gpio,
        .backlight_pin = lcd_backlight_gpio,
    };
    data_logger_display_init(&lcd_pins);

    printf("%s online (module_id=%u)\n", g_module_config.module_name, g_module_config.module_id);

    absolute_time_t next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    uint8_t heartbeat_counter = 0;
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;

        if (elapsed_lvgl_ms != 0U) {
            data_logger_display_tick(elapsed_lvgl_ms);
            last_lvgl_tick_ms = now_ms;
        }
        data_logger_display_task_handler();

        if (scp_can_try_read(&can_bus, &rx_msg)) {
            data_logger_display_append_can_event(&rx_msg, now_ms);
            printf("RX id=0x%lx dlc=%u\n", (unsigned long)rx_msg.id, (unsigned int)rx_msg.dlc);
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
