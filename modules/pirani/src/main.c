#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"

#include "module_config.h"
#include "pressure_display.h"
#include "pressure_sensor.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"

#define SENSOR_SAMPLE_PERIOD_MS 100
#define PRESSURE_DISCONNECTED_TORR 1000.0f
#define PRESSURE_CONNECTED_MIN_VOLTAGE 2.0f
#define PRESSURE_CONNECTED_MAX_VOLTAGE 10.0f

static pressure_display_unit_t pressure_unit_from_switch_state(bool switch_a_active, bool switch_b_active) {
    if (switch_a_active && !switch_b_active) {
        return PRESSURE_DISPLAY_UNIT_BAR;
    }
    if (!switch_a_active && switch_b_active) {
        return PRESSURE_DISPLAY_UNIT_VOLTAGE;
    }

    return PRESSURE_DISPLAY_UNIT_TORR;
}

static const char *pressure_unit_name(pressure_display_unit_t unit) {
    switch (unit) {
        case PRESSURE_DISPLAY_UNIT_BAR:
            return "bar";
        case PRESSURE_DISPLAY_UNIT_VOLTAGE:
            return "voltage";
        case PRESSURE_DISPLAY_UNIT_TORR:
        default:
            return "torr";
    }
}

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SCP_GPIO_SIGNAL_HEARTBEAT_LED, 25},

    {SCP_GPIO_SIGNAL_CAN_RX, 1},
    {SCP_GPIO_SIGNAL_CAN_TX, 0},

    {SCP_GPIO_SIGNAL_CONNECTION_OK, 17},
    {SCP_GPIO_SIGNAL_CONNECTION_ACTIVITY, 15},

    {SCP_GPIO_SIGNAL_LCD_SPI_SCK, 2},
    {SCP_GPIO_SIGNAL_LCD_SPI_TX, 3},
    {SCP_GPIO_SIGNAL_LCD_COMMAND, 4},
    {SCP_GPIO_SIGNAL_LCD_SPI_CSN, 5},
    {SCP_GPIO_SIGNAL_LCD_RESET, 6},
    {SCP_GPIO_SIGNAL_LCD_BACKLIGHT, 8},
    {SCP_GPIO_SIGNAL_PRESSURE_SENSOR_ADC, 28},
    {SCP_GPIO_SIGNAL_SWITCH_A, 13},
    {SCP_GPIO_SIGNAL_SWITCH_B, 14},
};
const scp_module_config_t g_module_config = {
    .module_name = "pirani",
    .module_id = 3,
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

    absolute_time_t next_heartbeat = nil_time;
    absolute_time_t next_sensor_sample = nil_time;
    uint8_t heartbeat_counter = 0;

    // gpio
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t heartbeat_led_gpio;
    uint8_t connection_ok_gpio;
    uint8_t connection_activity_gpio;
    uint8_t lcd_spi_sck_gpio;
    uint8_t lcd_spi_tx_gpio;
    uint8_t lcd_spi_csn_gpio;
    uint8_t lcd_command_gpio;
    uint8_t lcd_reset_gpio;
    uint8_t lcd_backlight_gpio;
    uint8_t pressure_sensor_adc_gpio;
    uint8_t switch_a_gpio;
    uint8_t switch_b_gpio;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CONNECTION_OK, &connection_ok_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CONNECTION_ACTIVITY, &connection_activity_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_TX, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_PRESSURE_SENSOR_ADC, &pressure_sensor_adc_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_A, &switch_a_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_B, &switch_b_gpio)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
        return 1;
    }

    gpio_init(heartbeat_led_gpio);
    gpio_set_dir(heartbeat_led_gpio, GPIO_OUT);
    gpio_put(heartbeat_led_gpio, 0);

    gpio_init(connection_ok_gpio);
    gpio_set_dir(connection_ok_gpio, GPIO_OUT);
    gpio_put(connection_ok_gpio, 0);

    gpio_init(connection_activity_gpio);
    gpio_set_dir(connection_activity_gpio, GPIO_OUT);
    gpio_put(connection_activity_gpio, 0);

    gpio_init(switch_a_gpio);
    gpio_set_dir(switch_a_gpio, GPIO_IN);
    gpio_pull_up(switch_a_gpio);

    gpio_init(switch_b_gpio);
    gpio_set_dir(switch_b_gpio, GPIO_IN);
    gpio_pull_up(switch_b_gpio);

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }

    pressure_sensor_init(pressure_sensor_adc_gpio);
    const pressure_display_spi_pins_t lcd_pins = {
        .spi_sck_pin = lcd_spi_sck_gpio,
        .spi_tx_pin = lcd_spi_tx_gpio,
        .spi_csn_pin = lcd_spi_csn_gpio,
        .command_pin = lcd_command_gpio,
        .reset_pin = lcd_reset_gpio,
        .backlight_pin = lcd_backlight_gpio,
    };
    pressure_display_init(&lcd_pins);

    // give time to connect to serial port
    // sleep_ms(ONLINE_MESSAGE_DELAY_MS);

    printf("%s online (module_id=%u, pressure_sensor_adc_gpio=%u, connection_ok_gpio=%u)\n",
           g_module_config.module_name,
           g_module_config.module_id,
           pressure_sensor_adc_gpio,
           connection_ok_gpio
    );

    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    next_sensor_sample = make_timeout_time_ms(SENSOR_SAMPLE_PERIOD_MS);
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());
    bool last_connection_ok = false;
    pressure_display_unit_t last_display_unit = PRESSURE_DISPLAY_UNIT_TORR;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;

        if (elapsed_lvgl_ms != 0U) {
            pressure_display_tick(elapsed_lvgl_ms);
            last_lvgl_tick_ms = now_ms;
        }
        pressure_display_task_handler();

        if (absolute_time_diff_us(now, next_sensor_sample) <= 0) {
            const pressure_sensor_reading_t pressure_reading = pressure_sensor_read_torr();
            const bool connection_ok = pressure_reading.voltage >= PRESSURE_CONNECTED_MIN_VOLTAGE
                                       && pressure_reading.voltage <= 10.3;
            const bool switch_a_active = !gpio_get(switch_a_gpio);
            const bool switch_b_active = !gpio_get(switch_b_gpio);
            const pressure_display_unit_t display_unit = pressure_unit_from_switch_state(switch_a_active, switch_b_active);
            const float display_torr = connection_ok ? pressure_reading.pressure_torr : PRESSURE_DISCONNECTED_TORR;
            gpio_put(connection_ok_gpio, connection_ok);

            if (connection_ok) {
                pressure_display_render(display_torr, pressure_reading.voltage, display_unit);
            } else {
                pressure_display_render_unplugged();
            }

            if (display_unit != last_display_unit) {
                printf("\nunit changed: %s\n", pressure_unit_name(display_unit));
                fflush(stdout);
                last_display_unit = display_unit;
            }

            if (connection_ok && !last_connection_ok) {
                build_connection_detected_event(&tx_msg, g_module_config.module_id, now_ms);
                (void) scp_can_transmit(&can_bus, &tx_msg);
                printf("\nconnection detected\n");
                fflush(stdout);
            } else if (!connection_ok && last_connection_ok) {
                build_connection_lost_event(&tx_msg, g_module_config.module_id, now_ms);
                (void) scp_can_transmit(&can_bus, &tx_msg);
                printf("\nconnection lost\n");
                fflush(stdout);
            }

            last_connection_ok = connection_ok;
            next_sensor_sample = make_timeout_time_ms(SENSOR_SAMPLE_PERIOD_MS);
        }

        // heartbeat
        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            printf(".");
            fflush(stdout);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        if (scp_can_try_read(&can_bus, &rx_msg)) {
            printf("RX id=0x%lx dlc=%u\n", (unsigned long) rx_msg.id, rx_msg.dlc);
        }

        sleep_us(10);
    }
}
