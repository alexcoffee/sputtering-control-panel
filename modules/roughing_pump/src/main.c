#include <stdbool.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "module_config.h"
#include "roughing_pump_can_messages.h"
#include "scp/can_messages.h"
#include "scp/can_bus.h"

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SCP_GPIO_SIGNAL_HEARTBEAT_LED, 25},

    {SCP_GPIO_SIGNAL_CAN_RX, 1},
    {SCP_GPIO_SIGNAL_CAN_TX, 0},

    {SCP_GPIO_SIGNAL_CONNECTION_OK, 10},
    {SCP_GPIO_SIGNAL_CONNECTION_ACTIVITY, 11},
    {SCP_GPIO_SIGNAL_CONNECTION_DETECT, 12},

    {SCP_GPIO_SIGNAL_SSR, 15},
    {SCP_GPIO_SIGNAL_SWITCH_A, 17},
};

const scp_module_config_t g_module_config = {
    .module_name = "roughing_pump",
    .module_id = 9,
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
    uint8_t heartbeat_counter = 0;

    // gpio
    uint8_t heartbeat_led_gpio;
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t connection_ok_gpio;
    uint8_t connection_activity_gpio;
    uint8_t connection_detect_gpio;
    uint8_t switch_gpio;
    uint8_t ssr_gpio;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }


    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CONNECTION_OK, &connection_ok_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CONNECTION_ACTIVITY, &connection_activity_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CONNECTION_DETECT, &connection_detect_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SWITCH_A, &switch_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_SSR, &ssr_gpio)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
        return 1;
    }

    gpio_init(heartbeat_led_gpio);
    gpio_set_dir(heartbeat_led_gpio, GPIO_OUT);
    gpio_put(heartbeat_led_gpio, 0);

    gpio_init(connection_ok_gpio);
    gpio_set_dir(connection_ok_gpio, GPIO_OUT);
    gpio_put(connection_ok_gpio, 0);

    gpio_init(connection_detect_gpio);
    gpio_set_dir(connection_detect_gpio, GPIO_IN);
    gpio_pull_up(connection_detect_gpio);

    gpio_init(connection_activity_gpio);
    gpio_set_dir(connection_activity_gpio, GPIO_OUT);
    gpio_put(connection_activity_gpio, 0);

    gpio_init(switch_gpio);
    gpio_set_dir(switch_gpio, GPIO_IN);
    gpio_pull_up(switch_gpio);

    gpio_init(ssr_gpio);
    gpio_set_dir(ssr_gpio, GPIO_OUT);
    gpio_put(ssr_gpio, 0);

    if (!scp_can_init(&can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      can_gpio_rx,
                      can_gpio_tx)) {
        return 2;
    }

    bool last_switch_state = !gpio_get(switch_gpio);
    bool connection_ok = !gpio_get(connection_detect_gpio);
    bool last_connection_ok = connection_ok;
    bool switch_candidate_state = last_switch_state;
    absolute_time_t switch_candidate_since = get_absolute_time();
    gpio_put(connection_ok_gpio, connection_ok);
    gpio_put(ssr_gpio, last_switch_state);
    gpio_put(connection_activity_gpio, last_switch_state);

    roughing_pump_build_switch_event(&tx_msg, last_switch_state, to_ms_since_boot(get_absolute_time()));
    (void) scp_can_transmit(&can_bus, &tx_msg);

    // give time to connect to serial port
    sleep_ms(ONLINE_MESSAGE_DELAY_MS);
    printf("%s online (module_id=%u, switch_gpio=%u, ssr_gpio=%u)\n",
           g_module_config.module_name,
           g_module_config.module_id,
           switch_gpio,
           ssr_gpio
    );

    if (connection_ok) {
        build_connection_detected_event(&tx_msg, g_module_config.module_id, to_ms_since_boot(get_absolute_time()));
        (void) scp_can_transmit(&can_bus, &tx_msg);
        printf("\nconnection detected\n");
        fflush(stdout);
    }

    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);

    while (true) {
        absolute_time_t now = get_absolute_time();
        bool switch_state = !gpio_get(switch_gpio);
        connection_ok = !gpio_get(connection_detect_gpio);
        uint32_t uptime_ms = to_ms_since_boot(now);

        gpio_put(connection_ok_gpio, connection_ok);
        if (connection_ok && !last_connection_ok) {
            build_connection_detected_event(&tx_msg, g_module_config.module_id, uptime_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            printf("\nconnection detected\n");
            fflush(stdout);
        } else if (!connection_ok && last_connection_ok) {
            build_connection_lost_event(&tx_msg, g_module_config.module_id, uptime_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            printf("\nconnection lost\n");
            fflush(stdout);
        }
        last_connection_ok = connection_ok;

        if (switch_state != switch_candidate_state) {
            switch_candidate_state = switch_state;
            switch_candidate_since = now;
        } else if (switch_candidate_state != last_switch_state
                   && absolute_time_diff_us(switch_candidate_since, now) >= SWITCH_DEBOUNCE_US) {
            last_switch_state = switch_candidate_state;
            gpio_put(ssr_gpio, last_switch_state);
            gpio_put(connection_activity_gpio, last_switch_state);

            roughing_pump_build_switch_event(&tx_msg, last_switch_state, uptime_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);

            printf("switch=%u -> ssr=%u\n", last_switch_state ? 1U : 0U, last_switch_state ? 1U : 0U);
        }

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&tx_msg, g_module_config.module_id, heartbeat_counter++, uptime_ms);
            (void) scp_can_transmit(&can_bus, &tx_msg);
            printf(".");
            fflush(stdout);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);

            gpio_put(heartbeat_led_gpio, 1);
            sleep_us(SCP_LED_FLASH_PULSE_US);
            gpio_put(heartbeat_led_gpio, 0);
        }

        if (scp_can_try_read(&can_bus, &rx_msg)) {
            printf("RX id=0x%lx dlc=%lx\n", (unsigned long) rx_msg.id, rx_msg.dlc);
        }

        sleep_us(10);
    }
}
