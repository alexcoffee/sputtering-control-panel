#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"

#include "turbo_pump_can_messages.h"
#include "current_display.h"
#include "current_sensor.h"
#include "module_config.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"
#include "scp/protocol.h"

#define SENSOR_SAMPLE_PERIOD_MS 100
#define POWER_TRANSMIT_PERIOD_MS 1000
#define POWER_DISCONNECTED_WATTS 0.0f
#define CURRENT_DISCONNECTED_AMPS 0.0f
#define SENSOR_CONNECTED_MIN_VOLTAGE 0.0f
#define SENSOR_CONNECTED_MAX_VOLTAGE 10.5f

static bool display_unit_from_protocol_value(uint8_t protocol_value, current_display_unit_t *unit_out) {
    if (unit_out == NULL) {
        return false;
    }

    switch (protocol_value) {
        case SCP_DISPLAY_UNIT_TORR:
            *unit_out = CURRENT_DISPLAY_UNIT_WATT;
            return true;
        case SCP_DISPLAY_UNIT_VOLTAGE:
            *unit_out = CURRENT_DISPLAY_UNIT_VOLTAGE;
            return true;
        default:
            return false;
    }
}

static const char *display_unit_name(current_display_unit_t unit) {
    switch (unit) {
        case CURRENT_DISPLAY_UNIT_VOLTAGE:
            return "voltage";
        case CURRENT_DISPLAY_UNIT_WATT:
        default:
            return "watts";
    }
}

static bool try_parse_set_display_unit_command(const struct can2040_msg *msg, current_display_unit_t *unit_out) {
    if (msg == NULL || unit_out == NULL) {
        return false;
    }
    if (msg->id != scp_protocol_command_msg_id(SCP_MODULE_ID_TURBO_PUMP)) {
        return false;
    }
    if (msg->dlc < 4U || msg->data[0] != SCP_PROTOCOL_VERSION || msg->data[2] != SCP_COMMAND_SET_DISPLAY_UNIT) {
        return false;
    }

    return display_unit_from_protocol_value(msg->data[3], unit_out);
}

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_HEARTBEAT_LED, 25},
    {SIGNAL_CAN_RX, 1},
    {SIGNAL_CAN_TX, 0},
    {SIGNAL_CONNECTION_OK, 11},
    {SIGNAL_CONNECTION_ACTIVITY, 12},

    // SWITCH = rocker switch connected to opto coupler.
    { SIGNAL_TURBO_IN_SWITCH_ENABLE, 17},
    { SIGNAL_TURBO_IN_SWITCH_LOW_SPEED,10},
    { SIGNAL_TURBO_IN_LOW_SPEED,13},
    { SIGNAL_TURBO_IN_START,14},
    { SIGNAL_TURBO_IN_FAULT,15},
    { SIGNAL_TURBO_OUT_ENABLE,21},
    { SIGNAL_TURBO_OUT_LOW_SPEED,18},


    {SIGNAL_LCD_SPI_SCK, 2},
    {SIGNAL_LCD_SDI, 3},
    {SIGNAL_LCD_COMMAND, 4},
    {SIGNAL_LCD_SPI_CSN, 5},
    {SIGNAL_LCD_RESET, 6},
    {SIGNAL_LCD_BACKLIGHT, 8},
    {SIGNAL_PRESSURE_SENSOR_ADC, 28},
};

const scp_module_config_t g_module_config = {
    .module_name = "turbo_pump",
    .module_id = SCP_MODULE_ID_TURBO_PUMP,
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
    absolute_time_t next_power_transmit = nil_time;
    uint8_t heartbeat_counter = 0;

    // gpio
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t heartbeat_led_gpio;
    uint8_t connection_ok_gpio;
    uint8_t connection_activity_gpio;
    uint8_t switch_enable_gpio;
    uint8_t switch_low_speed_gpio;
    uint8_t turbo_in_low_speed_gpio;
    uint8_t turbo_in_start_gpio;
    uint8_t turbo_in_fault_gpio;
    uint8_t turbo_pump_ssr_gpio;
    uint8_t turbo_pump_low_speed_gpio;
    uint8_t lcd_spi_sck_gpio;
    uint8_t lcd_spi_tx_gpio;
    uint8_t lcd_spi_csn_gpio;
    uint8_t lcd_command_gpio;
    uint8_t lcd_reset_gpio;
    uint8_t lcd_backlight_gpio;
    uint8_t current_sensor_adc_gpio;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CONNECTION_OK, &connection_ok_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CONNECTION_ACTIVITY, &connection_activity_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_IN_SWITCH_ENABLE, &switch_enable_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_IN_SWITCH_LOW_SPEED, &switch_low_speed_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_IN_LOW_SPEED, &turbo_in_low_speed_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_IN_START, &turbo_in_start_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_IN_FAULT, &turbo_in_fault_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_OUT_ENABLE, &turbo_pump_ssr_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TURBO_OUT_LOW_SPEED, &turbo_pump_low_speed_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SDI, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_PRESSURE_SENSOR_ADC, &current_sensor_adc_gpio)) {
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

    gpio_init(switch_low_speed_gpio);
    gpio_set_dir(switch_low_speed_gpio, GPIO_IN);
    gpio_pull_up(switch_low_speed_gpio);

    gpio_init(turbo_in_low_speed_gpio);
    gpio_set_dir(turbo_in_low_speed_gpio, GPIO_IN);
    gpio_pull_up(turbo_in_low_speed_gpio);

    gpio_init(turbo_in_start_gpio);
    gpio_set_dir(turbo_in_start_gpio, GPIO_IN);
    gpio_pull_up(turbo_in_start_gpio);

    gpio_init(turbo_in_fault_gpio);
    gpio_set_dir(turbo_in_fault_gpio, GPIO_IN);
    gpio_pull_up(turbo_in_fault_gpio);

    gpio_init(turbo_pump_ssr_gpio);
    gpio_set_dir(turbo_pump_ssr_gpio, GPIO_OUT);
    gpio_put(turbo_pump_ssr_gpio, 0);

    gpio_init(turbo_pump_low_speed_gpio);
    gpio_set_dir(turbo_pump_low_speed_gpio, GPIO_OUT);
    gpio_put(turbo_pump_low_speed_gpio, 0);

    bool last_switch_state = !gpio_get(switch_enable_gpio);
    bool switch_candidate_state = last_switch_state;
    absolute_time_t switch_candidate_since = get_absolute_time();
    gpio_put(turbo_pump_ssr_gpio, last_switch_state);

    bool last_low_speed_state = !gpio_get(switch_low_speed_gpio);
    bool low_speed_candidate_state = last_low_speed_state;
    absolute_time_t low_speed_candidate_since = get_absolute_time();
    gpio_put(turbo_pump_low_speed_gpio, last_low_speed_state);
    gpio_put(connection_activity_gpio, last_switch_state || last_low_speed_state);

    bool last_turbo_in_low_speed_state = !gpio_get(turbo_in_low_speed_gpio);
    bool turbo_in_low_speed_candidate_state = last_turbo_in_low_speed_state;
    absolute_time_t turbo_in_low_speed_candidate_since = get_absolute_time();

    bool last_turbo_in_start_state = !gpio_get(turbo_in_start_gpio);
    bool turbo_in_start_candidate_state = last_turbo_in_start_state;
    absolute_time_t turbo_in_start_candidate_since = get_absolute_time();

    bool last_turbo_in_fault_state = !gpio_get(turbo_in_fault_gpio);
    bool turbo_in_fault_candidate_state = last_turbo_in_fault_state;
    absolute_time_t turbo_in_fault_candidate_since = get_absolute_time();

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }
    scp_flash_can_target_init(&flash_target, g_module_config.module_id);

    current_sensor_init(current_sensor_adc_gpio);
    const pressure_display_spi_pins_t lcd_pins = {
        .spi_sck_pin = lcd_spi_sck_gpio,
        .spi_tx_pin = lcd_spi_tx_gpio,
        .spi_csn_pin = lcd_spi_csn_gpio,
        .command_pin = lcd_command_gpio,
        .reset_pin = lcd_reset_gpio,
        .backlight_pin = lcd_backlight_gpio,
    };
    current_display_init(&lcd_pins);
    current_display_set_turbo_state(last_switch_state, last_low_speed_state);

    // give time to connect to serial port
    // sleep_ms(ONLINE_MESSAGE_DELAY_MS);

    printf("%s online (module_id=%u, current_sensor_adc_gpio=%u, connection_ok_gpio=%u)\n",
           g_module_config.module_name,
           g_module_config.module_id,
           current_sensor_adc_gpio,
           connection_ok_gpio
    );
    printf("turbo inputs (active-low): low_speed=%u start=%u fault=%u\n",
           last_turbo_in_low_speed_state ? 1U : 0U,
           last_turbo_in_start_state ? 1U : 0U,
           last_turbo_in_fault_state ? 1U : 0U);
    fflush(stdout);

    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    next_sensor_sample = make_timeout_time_ms(SENSOR_SAMPLE_PERIOD_MS);
    next_power_transmit = make_timeout_time_ms(POWER_TRANSMIT_PERIOD_MS);
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());
    bool last_connection_ok = false;
    bool has_current_sample = false;
    bool last_power_connection_ok = false;
    float last_power_watts = POWER_DISCONNECTED_WATTS;
    current_display_unit_t display_unit = CURRENT_DISPLAY_UNIT_WATT;

    turbo_pump_build_switch_event(&tx_msg, last_switch_state, to_ms_since_boot(get_absolute_time()));
    (void) scp_can_transmit(&can_bus, &tx_msg);

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;
        const bool switch_state = !gpio_get(switch_enable_gpio);
        const bool low_speed_switch_state = !gpio_get(switch_low_speed_gpio);
        const bool turbo_in_low_speed_state = !gpio_get(turbo_in_low_speed_gpio);
        const bool turbo_in_start_state = !gpio_get(turbo_in_start_gpio);
        const bool turbo_in_fault_state = !gpio_get(turbo_in_fault_gpio);

        if (elapsed_lvgl_ms != 0U) {
            current_display_tick(elapsed_lvgl_ms);
            last_lvgl_tick_ms = now_ms;
        }
        current_display_task_handler();

        if (absolute_time_diff_us(now, next_sensor_sample) <= 0) {
            const current_sensor_reading_t current_reading = current_sensor_read_amps();
            const bool connection_ok = current_reading.voltage >= SENSOR_CONNECTED_MIN_VOLTAGE
                                       && current_reading.voltage <= SENSOR_CONNECTED_MAX_VOLTAGE;
            const float display_current_amps = connection_ok ? current_reading.current_amps : CURRENT_DISCONNECTED_AMPS;
            gpio_put(connection_ok_gpio, connection_ok);

            if (connection_ok) {
                current_display_render(display_current_amps, current_reading.voltage, display_unit);
            } else {
                current_display_render_unplugged();
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

            last_power_watts = connection_ok ? display_current_amps * TURBO_PUMP_BUS_VOLTAGE : POWER_DISCONNECTED_WATTS;
            last_power_connection_ok = connection_ok;
            has_current_sample = true;
            last_connection_ok = connection_ok;
            next_sensor_sample = make_timeout_time_ms(SENSOR_SAMPLE_PERIOD_MS);
        }

        if (has_current_sample && absolute_time_diff_us(now, next_power_transmit) <= 0) {
            build_power_reading_event(&tx_msg, g_module_config.module_id, last_power_watts, last_power_connection_ok);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            next_power_transmit = make_timeout_time_ms(POWER_TRANSMIT_PERIOD_MS);
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
            current_display_unit_t requested_unit = display_unit;
            if (try_parse_set_display_unit_command(&rx_msg, &requested_unit) && requested_unit != display_unit) {
                display_unit = requested_unit;
                printf("\nunit changed via CAN: %s\n", display_unit_name(display_unit));
                fflush(stdout);
            }
        }

        if (switch_state != switch_candidate_state) {
            switch_candidate_state = switch_state;
            switch_candidate_since = now;
        } else if (switch_candidate_state != last_switch_state
                   && absolute_time_diff_us(switch_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_switch_state = switch_candidate_state;
            gpio_put(turbo_pump_ssr_gpio, last_switch_state);
            gpio_put(connection_activity_gpio, last_switch_state || last_low_speed_state);
            current_display_set_turbo_state(last_switch_state, last_low_speed_state);
            turbo_pump_build_switch_event(&tx_msg, last_switch_state, now_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);

            printf("\nenable switch=%u -> turbo pump ssr=%u\n",
                   last_switch_state ? 1U : 0U,
                   last_switch_state ? 1U : 0U);
            fflush(stdout);
        }

        if (low_speed_switch_state != low_speed_candidate_state) {
            low_speed_candidate_state = low_speed_switch_state;
            low_speed_candidate_since = now;
        } else if (low_speed_candidate_state != last_low_speed_state
                   && absolute_time_diff_us(low_speed_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_low_speed_state = low_speed_candidate_state;
            gpio_put(turbo_pump_low_speed_gpio, last_low_speed_state);
            gpio_put(connection_activity_gpio, last_switch_state || last_low_speed_state);
            current_display_set_turbo_state(last_switch_state, last_low_speed_state);

            printf("\nlow speed switch=%u -> turbo pump low speed=%u\n",
                   last_low_speed_state ? 1U : 0U,
                   last_low_speed_state ? 1U : 0U);
            fflush(stdout);
        }

        if (turbo_in_low_speed_state != turbo_in_low_speed_candidate_state) {
            turbo_in_low_speed_candidate_state = turbo_in_low_speed_state;
            turbo_in_low_speed_candidate_since = now;
        } else if (turbo_in_low_speed_candidate_state != last_turbo_in_low_speed_state
                   && absolute_time_diff_us(turbo_in_low_speed_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_turbo_in_low_speed_state = turbo_in_low_speed_candidate_state;
            printf("\nturbo input low_speed=%u\n", last_turbo_in_low_speed_state ? 1U : 0U);
            fflush(stdout);
        }

        if (turbo_in_start_state != turbo_in_start_candidate_state) {
            turbo_in_start_candidate_state = turbo_in_start_state;
            turbo_in_start_candidate_since = now;
        } else if (turbo_in_start_candidate_state != last_turbo_in_start_state
                   && absolute_time_diff_us(turbo_in_start_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_turbo_in_start_state = turbo_in_start_candidate_state;
            printf("\nturbo input start=%u\n", last_turbo_in_start_state ? 1U : 0U);
            fflush(stdout);
        }

        if (turbo_in_fault_state != turbo_in_fault_candidate_state) {
            turbo_in_fault_candidate_state = turbo_in_fault_state;
            turbo_in_fault_candidate_since = now;
        } else if (turbo_in_fault_candidate_state != last_turbo_in_fault_state
                   && absolute_time_diff_us(turbo_in_fault_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_turbo_in_fault_state = turbo_in_fault_candidate_state;
            printf("\nturbo input fault=%u\n", last_turbo_in_fault_state ? 1U : 0U);
            fflush(stdout);
        }

        sleep_us(10);
    }
}
