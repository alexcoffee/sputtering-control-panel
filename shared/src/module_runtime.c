#include "scp/module_runtime.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "scp/can_bus.h"
#include "scp/protocol.h"

#define SCP_HEARTBEAT_PERIOD_MS 1000
#define SCP_LED_FLASH_PERIOD_MS 1500

static void scp_build_heartbeat(struct can2040_msg *msg,
                                const scp_module_config_t *cfg,
                                uint8_t state,
                                uint8_t heartbeat_counter,
                                uint32_t uptime_ms) {
    if (msg == NULL || cfg == NULL) {
        return;
    }

    msg->id = scp_protocol_heartbeat_msg_id(cfg->module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = cfg->module_id;
    msg->data[2] = state;
    msg->data[3] = heartbeat_counter;
    msg->data[4] = (uint8_t)(uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t)((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t)((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t)((uptime_ms >> 24) & 0xFFU);
}

bool scp_module_build_gpio_map(const scp_module_config_t *cfg, scp_pico_gpio_map_t *gpio_map) {
    char gpio_error[128];

    if (cfg == NULL || cfg->module_name == NULL || gpio_map == NULL) {
        return false;
    }

    if (!scp_pico_gpio_map_build(cfg->gpio_assignments,
                                 cfg->gpio_assignment_count,
                                 gpio_map,
                                 gpio_error,
                                 sizeof(gpio_error))) {
        printf("%s pin map error: %s\n", cfg->module_name, gpio_error);
        return false;
    }

    return true;
}

int scp_module_run(const scp_module_config_t *cfg) {
    scp_can_bus_t can_bus;
    scp_pico_gpio_map_t gpio_map;
    struct can2040_msg tx_msg;
    struct can2040_msg rx_msg;
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t led_gpio;
    uint8_t heartbeat_counter;
    absolute_time_t next_heartbeat;
    absolute_time_t next_led_flash;

    if (cfg == NULL || cfg->module_name == NULL) {
        return 1;
    }

    stdio_init_all();

    if (!scp_module_build_gpio_map(cfg, &gpio_map)) {
        return 2;
    }

    if (!scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_CAN_TX, &can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SCP_GPIO_SIGNAL_HEARTBEAT_LED, &led_gpio)) {
        printf("%s pin map error: missing required signal (CAN_RX/CAN_TX/LED)\n", cfg->module_name);
        return 2;
    }

    gpio_init(led_gpio);
    gpio_set_dir(led_gpio, GPIO_OUT);
    gpio_put(led_gpio, 0);

    if (!scp_can_init(&can_bus, cfg->can_pio_num, cfg->can_bitrate, can_gpio_rx, can_gpio_tx)) {
        return 3;
    }

    printf("%s online (module_id=%u)\n", cfg->module_name, cfg->module_id);

    heartbeat_counter = 0;
    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD_MS);
    next_led_flash = make_timeout_time_ms(SCP_LED_FLASH_PERIOD_MS);
    while (true) {
        absolute_time_t now = get_absolute_time();
        uint32_t uptime_ms = (uint32_t)to_ms_since_boot(get_absolute_time());

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            scp_build_heartbeat(&tx_msg, cfg, SCP_STATE_RUN, heartbeat_counter++, uptime_ms);
            (void)scp_can_transmit(&can_bus, &tx_msg);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD_MS);
        }

        if (absolute_time_diff_us(now, next_led_flash) <= 0) {
            gpio_put(led_gpio, 1);
            sleep_us(SCP_LED_FLASH_PULSE_US);
            gpio_put(led_gpio, 0);
            next_led_flash = make_timeout_time_ms(SCP_LED_FLASH_PERIOD_MS);
        }

        if (scp_can_try_read(&can_bus, &rx_msg)) {
            printf("RX id=0x%lx dlc=%u data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   (unsigned long)rx_msg.id,
                   rx_msg.dlc,
                   rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3],
                   rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
        }

        sleep_ms(1);
    }
}
