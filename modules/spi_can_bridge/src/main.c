#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "module_config.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"

#define SPI_CAN_PACKET_MAGIC_0 0xA5U
#define SPI_CAN_PACKET_MAGIC_1 0x5AU
#define SPI_CAN_PACKET_SIZE 16U
#define SPI_CAN_RESPONSE_QUEUE_CAPACITY 32U
#define SPI_CAN_TX_RETRY_TIMEOUT_US 2000U
#define SPI_CAN_LED_FLASH_PERIOD_MS 3000U

enum {
    SPI_CAN_PACKET_TYPE_IDLE = 0,
    SPI_CAN_PACKET_TYPE_CAN_TX = 1,
    SPI_CAN_PACKET_TYPE_PING = 2,
    SPI_CAN_PACKET_TYPE_SET_BITRATE = 3,
    SPI_CAN_PACKET_TYPE_STATUS = 128,
    SPI_CAN_PACKET_TYPE_CAN_RX = 129,
    SPI_CAN_PACKET_TYPE_PONG = 130
};

enum {
    SPI_CAN_STATUS_OK = 0,
    SPI_CAN_STATUS_TX_FAILED = 1,
    SPI_CAN_STATUS_BAD_PACKET = 2,
    SPI_CAN_STATUS_BAD_DLC = 3,
    SPI_CAN_STATUS_BAD_BITRATE = 4,
    SPI_CAN_STATUS_CAN_INIT_FAILED = 5
};

typedef struct {
    uint8_t head;
    uint8_t tail;
    uint8_t packets[SPI_CAN_RESPONSE_QUEUE_CAPACITY][SPI_CAN_PACKET_SIZE];
} spi_can_packet_queue_t;

typedef struct {
    bool valid;
    uint8_t packet[SPI_CAN_PACKET_SIZE];
} spi_can_pending_packet_t;

typedef struct {
    scp_can_bus_t can_bus;
    scp_flash_can_target_t flash_target;
    spi_can_packet_queue_t can_rx_queue;
    spi_can_pending_packet_t pending_control_response;
    uint8_t heartbeat_led_gpio;
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t host_spi_sck_gpio;
    uint8_t host_spi_tx_gpio;
    uint8_t host_spi_rx_gpio;
    uint8_t host_spi_csn_gpio;
    uint8_t heartbeat_counter;
    absolute_time_t next_heartbeat;
    repeating_timer_t led_flash_timer;
} spi_can_bridge_t;

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_HEARTBEAT_LED, 25},
    {SIGNAL_CAN_RX, 1},
    {SIGNAL_CAN_TX, 0},
    {SIGNAL_HOST_SPI_SCK, 18},
    {SIGNAL_HOST_SPI_TX, 19},
    {SIGNAL_HOST_SPI_RX, 16},
    {SIGNAL_HOST_SPI_CSN, 17},
};

const scp_module_config_t g_module_config = {
    .module_name = "spi_can_bridge",
    .module_id = SCP_MODULE_ID_SPI_CAN_BRIDGE,
    .can_pio_num = 0,
    .can_bitrate = SCP_CAN_BITRATE,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

static void spi_can_write_u16_le(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void spi_can_write_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t spi_can_read_u32_le(const uint8_t data[4]) {
    return (uint32_t)data[0]
           | ((uint32_t)data[1] << 8)
           | ((uint32_t)data[2] << 16)
           | ((uint32_t)data[3] << 24);
}

static void spi_can_queue_init(spi_can_packet_queue_t *queue) {
    if (queue == NULL) {
        return;
    }
    memset(queue, 0, sizeof(*queue));
}

static bool spi_can_queue_is_empty(const spi_can_packet_queue_t *queue) {
    return queue == NULL || queue->head == queue->tail;
}

static bool spi_can_queue_push(spi_can_packet_queue_t *queue, const uint8_t packet[SPI_CAN_PACKET_SIZE]) {
    uint8_t next;

    if (queue == NULL || packet == NULL) {
        return false;
    }

    next = (uint8_t)((queue->head + 1U) % SPI_CAN_RESPONSE_QUEUE_CAPACITY);
    if (next == queue->tail) {
        queue->tail = (uint8_t)((queue->tail + 1U) % SPI_CAN_RESPONSE_QUEUE_CAPACITY);
    }

    memcpy(queue->packets[queue->head], packet, SPI_CAN_PACKET_SIZE);
    queue->head = next;
    return true;
}

static bool spi_can_queue_pop(spi_can_packet_queue_t *queue, uint8_t packet_out[SPI_CAN_PACKET_SIZE]) {
    if (queue == NULL || packet_out == NULL || spi_can_queue_is_empty(queue)) {
        return false;
    }

    memcpy(packet_out, queue->packets[queue->tail], SPI_CAN_PACKET_SIZE);
    queue->tail = (uint8_t)((queue->tail + 1U) % SPI_CAN_RESPONSE_QUEUE_CAPACITY);
    return true;
}

static void spi_can_queue_control_response(spi_can_pending_packet_t *pending, const uint8_t packet[SPI_CAN_PACKET_SIZE]) {
    if (pending == NULL || packet == NULL) {
        return;
    }

    memcpy(pending->packet, packet, SPI_CAN_PACKET_SIZE);
    pending->valid = true;
}

static void spi_can_build_status_packet(uint8_t packet[SPI_CAN_PACKET_SIZE], uint8_t status_code, uint32_t argument) {
    if (packet == NULL) {
        return;
    }

    memset(packet, 0, SPI_CAN_PACKET_SIZE);
    packet[0] = SPI_CAN_PACKET_MAGIC_0;
    packet[1] = SPI_CAN_PACKET_MAGIC_1;
    packet[2] = SPI_CAN_PACKET_TYPE_STATUS;
    packet[3] = status_code;
    spi_can_write_u32_le(&packet[8], argument);
}

static void spi_can_build_pong_packet(uint8_t packet[SPI_CAN_PACKET_SIZE], const uint8_t request[SPI_CAN_PACKET_SIZE]) {
    if (packet == NULL || request == NULL) {
        return;
    }

    memset(packet, 0, SPI_CAN_PACKET_SIZE);
    packet[0] = SPI_CAN_PACKET_MAGIC_0;
    packet[1] = SPI_CAN_PACKET_MAGIC_1;
    packet[2] = SPI_CAN_PACKET_TYPE_PONG;
    packet[3] = request[3];
    memcpy(&packet[8], &request[8], 8U);
}

static void spi_can_build_can_rx_packet(uint8_t packet[SPI_CAN_PACKET_SIZE], const struct can2040_msg *msg) {
    if (packet == NULL || msg == NULL) {
        return;
    }

    memset(packet, 0, SPI_CAN_PACKET_SIZE);
    packet[0] = SPI_CAN_PACKET_MAGIC_0;
    packet[1] = SPI_CAN_PACKET_MAGIC_1;
    packet[2] = SPI_CAN_PACKET_TYPE_CAN_RX;
    spi_can_write_u16_le(&packet[4], (uint16_t)(msg->id & 0x7FFU));
    packet[6] = (uint8_t)(msg->dlc & 0x0FU);
    for (uint8_t i = 0U; i < 8U; ++i) {
        packet[8U + i] = msg->data[i];
    }
}

static bool spi_can_packet_is_valid_request(const uint8_t packet[SPI_CAN_PACKET_SIZE]) {
    return packet != NULL
           && packet[0] == SPI_CAN_PACKET_MAGIC_0
           && packet[1] == SPI_CAN_PACKET_MAGIC_1;
}

static bool can_transmit_with_retry(scp_can_bus_t *can_bus, struct can2040_msg *tx_msg, uint32_t timeout_us) {
    if (can_bus == NULL || tx_msg == NULL) {
        return false;
    }

    const absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!time_reached(deadline)) {
        if (scp_can_transmit(can_bus, tx_msg)) {
            return true;
        }
        sleep_us(10);
    }

    return scp_can_transmit(can_bus, tx_msg);
}

static bool should_forward_can_rx_to_host(const struct can2040_msg *msg) {
    if (msg == NULL) {
        return false;
    }

    if (msg->id >= SCP_MSG_FLASH_CONTROL_BASE && msg->id < SCP_MSG_FLASH_STATUS_BASE) {
        return false;
    }

    return true;
}

static void spi_can_handle_request(spi_can_bridge_t *bridge, const uint8_t request[SPI_CAN_PACKET_SIZE]) {
    uint8_t response[SPI_CAN_PACKET_SIZE];

    if (bridge == NULL || request == NULL || !spi_can_packet_is_valid_request(request)) {
        return;
    }

    switch (request[2]) {
        case SPI_CAN_PACKET_TYPE_IDLE:
            return;
        case SPI_CAN_PACKET_TYPE_CAN_TX: {
            struct can2040_msg tx_msg = {0};

            tx_msg.id = ((uint32_t)request[4] | ((uint32_t)request[5] << 8)) & 0x7FFU;
            tx_msg.dlc = (uint8_t)(request[6] & 0x0FU);
            if (tx_msg.dlc > 8U) {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_BAD_DLC, tx_msg.dlc);
                spi_can_queue_control_response(&bridge->pending_control_response, response);
                return;
            }

            for (uint8_t i = 0U; i < 8U; ++i) {
                tx_msg.data[i] = request[8U + i];
            }

            if (!can_transmit_with_retry(&bridge->can_bus, &tx_msg, SPI_CAN_TX_RETRY_TIMEOUT_US)) {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_TX_FAILED, tx_msg.id);
            } else {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_OK, tx_msg.id);
            }
            spi_can_queue_control_response(&bridge->pending_control_response, response);
            return;
        }
        case SPI_CAN_PACKET_TYPE_PING:
            spi_can_build_pong_packet(response, request);
            spi_can_queue_control_response(&bridge->pending_control_response, response);
            return;
        case SPI_CAN_PACKET_TYPE_SET_BITRATE: {
            const uint32_t requested_bitrate = spi_can_read_u32_le(&request[8]);

            if (requested_bitrate < 125000U || requested_bitrate > 1000000U) {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_BAD_BITRATE, requested_bitrate);
                spi_can_queue_control_response(&bridge->pending_control_response, response);
                return;
            }

            if (!scp_can_init(&bridge->can_bus,
                              g_module_config.can_pio_num,
                              requested_bitrate,
                              bridge->can_gpio_rx,
                              bridge->can_gpio_tx)) {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_CAN_INIT_FAILED, requested_bitrate);
            } else {
                spi_can_build_status_packet(response, SPI_CAN_STATUS_OK, requested_bitrate);
            }
            spi_can_queue_control_response(&bridge->pending_control_response, response);
            return;
        }
        default:
            spi_can_build_status_packet(response, SPI_CAN_STATUS_BAD_PACKET, request[2]);
            spi_can_queue_control_response(&bridge->pending_control_response, response);
            return;
    }
}

static void spi_can_service_can_bus(spi_can_bridge_t *bridge) {
    struct can2040_msg rx_msg;

    if (bridge == NULL) {
        return;
    }

    while (scp_can_try_read(&bridge->can_bus, &rx_msg)) {
        if (scp_flash_can_target_handle_can_frame(&bridge->flash_target, &bridge->can_bus, &rx_msg)) {
            continue;
        }
        if (should_forward_can_rx_to_host(&rx_msg)) {
            uint8_t packet[SPI_CAN_PACKET_SIZE];
            spi_can_build_can_rx_packet(packet, &rx_msg);
            spi_can_queue_push(&bridge->can_rx_queue, packet);
        }
    }
}

static void spi_can_queue_can_rx_msg(spi_can_bridge_t *bridge, const struct can2040_msg *msg) {
    uint8_t packet[SPI_CAN_PACKET_SIZE];

    if (bridge == NULL || msg == NULL) {
        return;
    }

    spi_can_build_can_rx_packet(packet, msg);
    (void)spi_can_queue_push(&bridge->can_rx_queue, packet);
}

static void spi_can_build_heartbeat(struct can2040_msg *msg,
                                    uint8_t module_id,
                                    uint8_t counter,
                                    uint32_t uptime_ms) {
    if (msg == NULL) {
        return;
    }

    msg->id = scp_protocol_heartbeat_msg_id(module_id);
    msg->dlc = 8U;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_STATE_RUN;
    msg->data[3] = counter;
    msg->data[4] = (uint8_t)(uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t)((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t)((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t)((uptime_ms >> 24) & 0xFFU);
}

static int64_t spi_can_led_flash_off_callback(alarm_id_t id, void *user_data) {
    (void)id;

    if (user_data == NULL) {
        return 0;
    }

    gpio_put(*(const uint8_t *)user_data, 0);
    return 0;
}

static bool spi_can_led_flash_timer_callback(repeating_timer_t *timer) {
    if (timer == NULL || timer->user_data == NULL) {
        return true;
    }

    uint8_t gpio = *(const uint8_t *)timer->user_data;
    gpio_put(gpio, 1);
    if (add_alarm_in_us(SCP_LED_FLASH_PULSE_US, spi_can_led_flash_off_callback, timer->user_data, false) < 0) {
        gpio_put(gpio, 0);
    }
    return true;
}

static void spi_can_exchange_with_host(spi_can_bridge_t *bridge) {
    uint8_t tx_packet[SPI_CAN_PACKET_SIZE] = {0};
    uint8_t rx_packet[SPI_CAN_PACKET_SIZE] = {0};

    if (bridge == NULL) {
        return;
    }

    if (bridge->pending_control_response.valid) {
        memcpy(tx_packet, bridge->pending_control_response.packet, SPI_CAN_PACKET_SIZE);
        bridge->pending_control_response.valid = false;
    } else {
        (void)spi_can_queue_pop(&bridge->can_rx_queue, tx_packet);
    }

    (void)spi_write_read_blocking(spi0, tx_packet, rx_packet, SPI_CAN_PACKET_SIZE);
    spi_can_handle_request(bridge, rx_packet);
}

static void spi_can_bridge_init(spi_can_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    memset(bridge, 0, sizeof(*bridge));
    spi_can_queue_init(&bridge->can_rx_queue);
    bridge->next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
}

int main(void) {
    spi_can_bridge_t bridge;
    scp_pico_gpio_map_t gpio_map;
    struct can2040_msg heartbeat_msg;

    stdio_init_all();
    (void)scp_bootloader_run_if_requested(&g_module_config, SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS);

    spi_can_bridge_init(&bridge);

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &bridge.heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &bridge.can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &bridge.can_gpio_tx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HOST_SPI_SCK, &bridge.host_spi_sck_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HOST_SPI_TX, &bridge.host_spi_tx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HOST_SPI_RX, &bridge.host_spi_rx_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HOST_SPI_CSN, &bridge.host_spi_csn_gpio)) {
        printf("%s pin map error: missing required signal\n", g_module_config.module_name);
        return 1;
    }

    gpio_init(bridge.heartbeat_led_gpio);
    gpio_set_dir(bridge.heartbeat_led_gpio, GPIO_OUT);
    gpio_put(bridge.heartbeat_led_gpio, 0);
    add_repeating_timer_ms((int32_t)SPI_CAN_LED_FLASH_PERIOD_MS,
                           spi_can_led_flash_timer_callback,
                           &bridge.heartbeat_led_gpio,
                           &bridge.led_flash_timer);

    gpio_set_function(bridge.host_spi_sck_gpio, GPIO_FUNC_SPI);
    gpio_set_function(bridge.host_spi_tx_gpio, GPIO_FUNC_SPI);
    gpio_set_function(bridge.host_spi_rx_gpio, GPIO_FUNC_SPI);
    gpio_set_function(bridge.host_spi_csn_gpio, GPIO_FUNC_SPI);
    gpio_pull_up(bridge.host_spi_csn_gpio);
    spi_init(spi0, 1000000U);
    spi_set_slave(spi0, true);
    spi_set_format(spi0, 8U, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    if (!scp_can_init(&bridge.can_bus,
                      g_module_config.can_pio_num,
                      g_module_config.can_bitrate,
                      bridge.can_gpio_rx,
                      bridge.can_gpio_tx)) {
        return 2;
    }
    scp_flash_can_target_init(&bridge.flash_target, g_module_config.module_id);

    printf("%s online (module_id=%u)\n", g_module_config.module_name, g_module_config.module_id);

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t uptime_ms = to_ms_since_boot(now);

        if (absolute_time_diff_us(now, bridge.next_heartbeat) <= 0) {
            spi_can_build_heartbeat(&heartbeat_msg, g_module_config.module_id, bridge.heartbeat_counter++, uptime_ms);
            (void)scp_can_transmit(&bridge.can_bus, &heartbeat_msg);
            bridge.next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        spi_can_service_can_bus(&bridge);
        spi_can_exchange_with_host(&bridge);
    }
}
