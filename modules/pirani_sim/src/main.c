#include <stdbool.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "module_config.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"

#define PIRANI_SIM_PRESSURE_PERIOD_MS 250U
#define PIRANI_SIM_PRESSURE_CYCLE_MS 30000U
#define PIRANI_SIM_PRESSURE_ACTIVE_MS 26000U
#define PIRANI_SIM_PRESSURE_MIN_TORR 1.0e-3f
#define PIRANI_SIM_PRESSURE_MAX_TORR 7.6e2f

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_CAN_RX, 1},
    {SIGNAL_CAN_TX, 0},
};

const scp_module_config_t g_module_config = {
    .module_name = "pirani_sim",
    .module_id = SCP_MODULE_ID_PIRANI_SIM,
    .can_pio_num = 0,
    .can_bitrate = SCP_CAN_BITRATE,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

static float pirani_simulate_pressure_torr(uint32_t uptime_ms, bool *connection_ok_out) {
    const uint32_t phase_ms = uptime_ms % PIRANI_SIM_PRESSURE_CYCLE_MS;
    const bool connection_ok = phase_ms < PIRANI_SIM_PRESSURE_ACTIVE_MS;
    if (connection_ok_out != NULL) {
        *connection_ok_out = connection_ok;
    }
    if (!connection_ok) {
        return 0.0f;
    }

    const uint32_t sweep_ms = PIRANI_SIM_PRESSURE_ACTIVE_MS / 2U;
    const float ramp = phase_ms < sweep_ms
                           ? (float)phase_ms / (float)sweep_ms
                           : (float)(PIRANI_SIM_PRESSURE_ACTIVE_MS - phase_ms) / (float)sweep_ms;
    return PIRANI_SIM_PRESSURE_MIN_TORR
           + ramp * (PIRANI_SIM_PRESSURE_MAX_TORR - PIRANI_SIM_PRESSURE_MIN_TORR);
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

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
        return 1;
    }

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }
    scp_flash_can_target_init(&flash_target, g_module_config.module_id);

    printf("%s online (module_id=%u, can_gpio_rx=%u, can_gpio_tx=%u)\n",
           g_module_config.module_name,
           g_module_config.module_id,
           can_gpio_rx,
           can_gpio_tx);

    sleep_ms(ONLINE_MESSAGE_DELAY_MS);

    absolute_time_t next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    absolute_time_t next_pressure_sample = make_timeout_time_ms(0U);
    uint8_t heartbeat_counter = 0U;
    bool last_connection_ok = false;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t uptime_ms = to_ms_since_boot(now);
        bool flash_active = flash_target.session_active || flash_target.image_ready;

        while (scp_can_try_read(&can_bus, &rx_msg)) {
            (void)scp_flash_can_target_handle_can_frame(&flash_target, &can_bus, &rx_msg);
        }

        if (!flash_active && absolute_time_diff_us(now, next_pressure_sample) <= 0) {
            bool connection_ok = false;
            const float pressure_torr = pirani_simulate_pressure_torr(uptime_ms, &connection_ok);

            if (connection_ok && !last_connection_ok) {
                build_connection_detected_event(&tx_msg, g_module_config.module_id, uptime_ms);
                (void)scp_can_transmit(&can_bus, &tx_msg);
            } else if (!connection_ok && last_connection_ok) {
                build_connection_lost_event(&tx_msg, g_module_config.module_id, uptime_ms);
                (void)scp_can_transmit(&can_bus, &tx_msg);
            }
            last_connection_ok = connection_ok;

            build_pressure_reading_event(&tx_msg, g_module_config.module_id, pressure_torr, connection_ok);
            (void)scp_can_transmit(&can_bus, &tx_msg);
            next_pressure_sample = make_timeout_time_ms(PIRANI_SIM_PRESSURE_PERIOD_MS);
        }

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, uptime_ms);
            (void)scp_can_transmit(&can_bus, &tx_msg);
            printf(".");
            fflush(stdout);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        sleep_us(10);
    }
}
