#include <stdio.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "module_config.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"
#include "touch_calibrator_display.h"

#define MONITOR_CAN_METRICS_PERIOD_MS 250U
#define MONITOR_CAN_RX_BUDGET_UI_LOOP 24U
#define MONITOR_CAN_RX_BUDGET_FLASH_LOOP 96U

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_HEARTBEAT_LED, 25},

    {SIGNAL_CAN_TX, 0},
    {SIGNAL_CAN_RX, 1},

    {SIGNAL_LCD_SPI_CSN, 2},
    {SIGNAL_LCD_RESET, 3},
    {SIGNAL_LCD_SDO, 4},
    {SIGNAL_LCD_COMMAND, 5},
    {SIGNAL_LCD_SPI_SCK, 6},
    {SIGNAL_LCD_SDI, 7},
    {SIGNAL_LCD_BACKLIGHT, 8},
    {SIGNAL_TOUCH_SPI_CSN, 9},
    {SIGNAL_TOUCH_SPI_SCK, 10},
    {SIGNAL_TOUCH_SDI, 11},
    {SIGNAL_TOUCH_SDO, 12},
    {SIGNAL_TOUCH_IRQ, 13},
    {SIGNAL_SWITCH_A, 14},
    {SIGNAL_SWITCH_B, 15},
    {SIGNAL_SWITCH_ENABLE, 16},
};

const scp_module_config_t g_module_config = {
    .module_name = "monitor",
    .module_id = SCP_MODULE_ID_MONITOR,
    .can_pio_num = 0,
    .can_bitrate = SCP_CAN_BITRATE,
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
    (void)scp_bootloader_run_if_requested(&g_module_config, SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS);

    scp_can_bus_t can_bus;
    scp_flash_can_target_t flash_target;
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
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_SCK, &lcd_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SDI, &lcd_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SDO, &lcd_spi_rx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_SPI_CSN, &lcd_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_COMMAND, &lcd_command_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_RESET, &lcd_reset_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_LCD_BACKLIGHT, &lcd_backlight_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TOUCH_SPI_SCK, &touch_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TOUCH_SDI, &touch_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TOUCH_SDO, &touch_spi_rx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TOUCH_SPI_CSN, &touch_spi_csn_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_TOUCH_IRQ, &touch_irq_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_SWITCH_A, &encoder_a_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_SWITCH_B, &encoder_b_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_SWITCH_ENABLE, &encoder_button_gpio)) {
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
    scp_flash_can_target_init(&flash_target, g_module_config.module_id);

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
    absolute_time_t next_can_metrics_update = make_timeout_time_ms(0U);
    uint8_t heartbeat_counter = 0;
    uint32_t last_lvgl_tick_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_can_retransmit_count = 0U;
    bool can_retransmit_count_valid = false;
    uint32_t can_retransmit_count = 0U;
    struct can2040_stats prev_can_stats = {0U, 0U, 0U, 0U};
    bool prev_can_stats_valid = false;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        bool flash_active = flash_target.session_active || flash_target.image_ready;
        bool can_rx_budget_exhausted = false;
        uint16_t can_frames_processed = 0U;

        while (scp_can_try_read(&can_bus, &rx_msg)) {
            if (scp_flash_can_target_handle_can_frame(&flash_target, &can_bus, &rx_msg)) {
                flash_active = flash_target.session_active || flash_target.image_ready;
            } else if (!flash_active) {
                touch_calibrator_display_handle_can_message(&rx_msg, now_ms);
            }

            can_frames_processed++;
            const uint16_t can_rx_budget = flash_active ? MONITOR_CAN_RX_BUDGET_FLASH_LOOP : MONITOR_CAN_RX_BUDGET_UI_LOOP;
            if (can_frames_processed >= can_rx_budget) {
                can_rx_budget_exhausted = true;
                break;
            }
        }

        if (!flash_active) {
            const uint32_t elapsed_lvgl_ms = now_ms - last_lvgl_tick_ms;
            if (elapsed_lvgl_ms != 0U) {
                touch_calibrator_display_tick(elapsed_lvgl_ms);
                last_lvgl_tick_ms = now_ms;
            }
            touch_calibrator_display_task_handler();

            if (absolute_time_diff_us(now, next_can_metrics_update) <= 0) {
                struct can2040_stats can_stats;
                can2040_get_statistics(&can_bus.can, &can_stats);

                if (prev_can_stats_valid) {
                    if (can_stats.tx_attempt >= prev_can_stats.tx_attempt
                        && can_stats.tx_total >= prev_can_stats.tx_total) {
                        const uint32_t delta_tx_attempt = can_stats.tx_attempt - prev_can_stats.tx_attempt;
                        const uint32_t delta_tx_total = can_stats.tx_total - prev_can_stats.tx_total;
                        if (delta_tx_total > 0U && delta_tx_attempt > delta_tx_total) {
                            can_retransmit_count += delta_tx_attempt - delta_tx_total;
                        }
                    }
                }
                prev_can_stats = can_stats;
                prev_can_stats_valid = true;

                if (!can_retransmit_count_valid || can_retransmit_count != last_can_retransmit_count) {
                    touch_calibrator_display_set_can_retransmit_count(can_retransmit_count);
                    last_can_retransmit_count = can_retransmit_count;
                    can_retransmit_count_valid = true;
                }
                next_can_metrics_update = make_timeout_time_ms(MONITOR_CAN_METRICS_PERIOD_MS);
            }

            uint8_t requested_display_unit = 0U;
            if (touch_calibrator_display_take_pressure_unit_command(&requested_display_unit)) {
                build_set_display_unit_command(&tx_msg,
                                               g_module_config.module_id,
                                               SCP_MODULE_ID_ION_GAUGE,
                                               requested_display_unit);
                (void) scp_can_transmit(&can_bus, &tx_msg);

                build_set_display_unit_command(&tx_msg,
                                               g_module_config.module_id,
                                               SCP_MODULE_ID_PIRANI,
                                               requested_display_unit);
                (void) scp_can_transmit(&can_bus, &tx_msg);
            }

            if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
                build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
                (void)scp_can_transmit(&can_bus, &tx_msg);
                next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);

                gpio_put(heartbeat_led_gpio, 1);
                sleep_us(SCP_LED_FLASH_PULSE_US);
                gpio_put(heartbeat_led_gpio, 0);
            }

            sleep_us(can_rx_budget_exhausted ? 20U : 100U);
        } else {
            if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
                build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
                (void)scp_can_transmit(&can_bus, &tx_msg);
                next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
            }
            sleep_us(can_rx_budget_exhausted ? 10U : 20U);
        }
    }
}
