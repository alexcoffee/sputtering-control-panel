#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"

#include "ion_gauge_can_messages.h"
#include "module_config.h"
#include "pressure_display.h"
#include "pressure_sensor.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"
#include "scp/protocol.h"

#define SENSOR_SAMPLE_PERIOD_MS 100
#define PRESSURE_TRANSMIT_PERIOD_MS 1000
#define PRESSURE_DISCONNECTED_TORR 1000.0f
#define PRESSURE_CONNECTED_MIN_VOLTAGE 2.0f
#define PRESSURE_CONNECTED_MAX_VOLTAGE 10.0f

static bool pressure_unit_from_protocol_value(uint8_t protocol_value, pressure_display_unit_t *unit_out) {
    if (unit_out == NULL) {
        return false;
    }

    switch (protocol_value) {
        case SCP_DISPLAY_UNIT_TORR:
            *unit_out = PRESSURE_DISPLAY_UNIT_TORR;
            return true;
        case SCP_DISPLAY_UNIT_BAR:
            *unit_out = PRESSURE_DISPLAY_UNIT_BAR;
            return true;
        case SCP_DISPLAY_UNIT_VOLTAGE:
            *unit_out = PRESSURE_DISPLAY_UNIT_VOLTAGE;
            return true;
        default:
            return false;
    }
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

static bool try_parse_set_display_unit_command(const struct can2040_msg *msg, pressure_display_unit_t *unit_out) {
    if (msg == NULL || unit_out == NULL) {
        return false;
    }
    if (msg->id != scp_protocol_command_msg_id(SCP_MODULE_ID_ION_GAUGE)) {
        return false;
    }
    if (msg->dlc < 4U || msg->data[0] != SCP_PROTOCOL_VERSION || msg->data[2] != SCP_COMMAND_SET_DISPLAY_UNIT) {
        return false;
    }

    return pressure_unit_from_protocol_value(msg->data[3], unit_out);
}
#define SIGNAL_CONNECTION_OK "CONNECTION_OK"
static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_HEARTBEAT_LED, 25},

    {SIGNAL_CAN_RX, 1},
    {SIGNAL_CAN_TX, 0},

    {SIGNAL_CONNECTION_OK, 17},
    {SIGNAL_CONNECTION_ACTIVITY, 15},
    {SIGNAL_SWITCH_ENABLE, 16},
    {SIGNAL_SSR, 14},

    {SIGNAL_LCD_SPI_SCK, 2},
    {SIGNAL_LCD_SDI, 3},
    {SIGNAL_LCD_COMMAND, 4},
    {SIGNAL_LCD_SPI_CSN, 5},
    {SIGNAL_LCD_RESET, 6},
    {SIGNAL_LCD_BACKLIGHT, 8},
    {SIGNAL_PRESSURE_SENSOR_ADC, 28},
};

const scp_module_config_t g_module_config = {
    .module_name = "ion_gauge",
    .module_id = SCP_MODULE_ID_ION_GAUGE,
    .can_pio_num = 0,
    .can_bitrate = SCP_CAN_BITRATE,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

int main(void) {
    stdio_init_all();
    (void)scp_bootloader_run_if_requested(&g_module_config, SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS);

    scp_can_bus_t can_bus;
    scp_flash_can_target_t flash_target;
    scp_pico_gpio_map_t gpio_map;
    struct can2040_msg tx_msg;
    struct can2040_msg rx_msg;

    absolute_time_t next_heartbeat = nil_time;
    absolute_time_t next_sensor_sample = nil_time;
    absolute_time_t next_pressure_transmit = nil_time;
    uint8_t heartbeat_counter = 0;

    // gpio
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t heartbeat_led_gpio;
    uint8_t connection_ok_gpio;
    uint8_t connection_activity_gpio;
    uint8_t switch_enable_gpio;
    uint8_t pirani_opto_gpio;
    uint8_t lcd_spi_sck_gpio;
    uint8_t lcd_spi_tx_gpio;
    uint8_t lcd_spi_csn_gpio;
    uint8_t lcd_command_gpio;
    uint8_t lcd_reset_gpio;
    uint8_t lcd_backlight_gpio;
    uint8_t pressure_sensor_adc_gpio;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CONNECTION_OK, &connection_ok_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CONNECTION_ACTIVITY, &connection_activity_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_SWITCH_ENABLE, &switch_enable_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_SSR, &pirani_opto_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SDI, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_PRESSURE_SENSOR_ADC, &pressure_sensor_adc_gpio)) {
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

    gpio_init(switch_enable_gpio);
    gpio_set_dir(switch_enable_gpio, GPIO_IN);
    gpio_pull_up(switch_enable_gpio);

    gpio_init(pirani_opto_gpio);
    gpio_set_dir(pirani_opto_gpio, GPIO_OUT);
    gpio_put(pirani_opto_gpio, 0);

    bool last_switch_state = !gpio_get(switch_enable_gpio);
    bool switch_candidate_state = last_switch_state;
    absolute_time_t switch_candidate_since = get_absolute_time();
    gpio_put(pirani_opto_gpio, last_switch_state);

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }
    scp_flash_can_target_init(&flash_target, g_module_config.module_id);

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
    next_pressure_transmit = make_timeout_time_ms(PRESSURE_TRANSMIT_PERIOD_MS);
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());
    bool last_connection_ok = false;
    bool has_pressure_sample = false;
    bool last_pressure_connection_ok = false;
    float last_pressure_torr = PRESSURE_DISCONNECTED_TORR;
    pressure_display_unit_t display_unit = PRESSURE_DISPLAY_UNIT_TORR;

    ion_gauge_build_switch_event(&tx_msg, last_switch_state, to_ms_since_boot(get_absolute_time()));
    (void) scp_can_transmit(&can_bus, &tx_msg);

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;
        const bool switch_state = !gpio_get(switch_enable_gpio);

        if (elapsed_lvgl_ms != 0U) {
            pressure_display_tick(elapsed_lvgl_ms);
            last_lvgl_tick_ms = now_ms;
        }
        pressure_display_task_handler();

        if (absolute_time_diff_us(now, next_sensor_sample) <= 0) {
            const pressure_sensor_reading_t pressure_reading = pressure_sensor_read_torr();
            const bool connection_ok = pressure_reading.voltage >= PRESSURE_CONNECTED_MIN_VOLTAGE
                                       && pressure_reading.voltage <= PRESSURE_CONNECTED_MAX_VOLTAGE;
            const float display_torr = connection_ok ? pressure_reading.pressure_torr : PRESSURE_DISCONNECTED_TORR;
            gpio_put(connection_ok_gpio, connection_ok);

            if (connection_ok) {
                pressure_display_render(display_torr, pressure_reading.voltage, display_unit);
            } else {
                pressure_display_render_unplugged();
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

            last_pressure_torr = display_torr;
            last_pressure_connection_ok = connection_ok;
            has_pressure_sample = true;
            last_connection_ok = connection_ok;
            next_sensor_sample = make_timeout_time_ms(SENSOR_SAMPLE_PERIOD_MS);
        }

        if (has_pressure_sample && absolute_time_diff_us(now, next_pressure_transmit) <= 0) {
            build_pressure_reading_event(&tx_msg, g_module_config.module_id, last_pressure_torr, last_pressure_connection_ok);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            next_pressure_transmit = make_timeout_time_ms(PRESSURE_TRANSMIT_PERIOD_MS);
        }

        // heartbeat
        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            printf(".");
            fflush(stdout);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        while (scp_can_try_read(&can_bus, &rx_msg)) {
            if (scp_flash_can_target_handle_can_frame(&flash_target, &can_bus, &rx_msg)) {
                continue;
            }
            pressure_display_unit_t requested_unit = display_unit;
            if (try_parse_set_display_unit_command(&rx_msg, &requested_unit) && requested_unit != display_unit) {
                display_unit = requested_unit;
                printf("\nunit changed via CAN: %s\n", pressure_unit_name(display_unit));
                fflush(stdout);
            }
        }

        if (switch_state != switch_candidate_state) {
            switch_candidate_state = switch_state;
            switch_candidate_since = now;
        } else if (switch_candidate_state != last_switch_state
                   && absolute_time_diff_us(switch_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_switch_state = switch_candidate_state;
            gpio_put(pirani_opto_gpio, last_switch_state);
            ion_gauge_build_switch_event(&tx_msg, last_switch_state, now_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);

            printf("\nenable switch=%u -> pirani opto=%u\n",
                   last_switch_state ? 1U : 0U,
                   last_switch_state ? 1U : 0U);
            fflush(stdout);
        }

        sleep_us(10);
    }
}
