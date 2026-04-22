#include <stdio.h>

#include "pico/stdlib.h"

#include "module_config.h"
#include "scp/bootloader.h"
#include "scp/can_bus.h"
#include "scp/can_messages.h"
#include "scp/flash_can.h"
#include "scp/module_ids.h"

/* Provided by pico_stdio_usb when USB stdio is enabled for this target. */
bool stdio_usb_connected(void);

#define USB_CAN_PACKET_MAGIC_0 0xA5U
#define USB_CAN_PACKET_MAGIC_1 0x5AU
#define USB_CAN_PACKET_SIZE 16U
#define USB_CAN_MAX_USB_BYTES_PER_LOOP 128U
#define USB_CAN_MAX_CAN_RX_FORWARD_PER_LOOP 12U
#define USB_CAN_TX_RETRY_TIMEOUT_US 2000U

enum {
    USB_CAN_PACKET_TYPE_CAN_TX = 1,
    USB_CAN_PACKET_TYPE_PING = 2,
    USB_CAN_PACKET_TYPE_SET_BITRATE = 3,
    USB_CAN_PACKET_TYPE_STATUS = 128,
    USB_CAN_PACKET_TYPE_CAN_RX = 129,
    USB_CAN_PACKET_TYPE_PONG = 130
};

enum {
    USB_CAN_STATUS_OK = 0,
    USB_CAN_STATUS_TX_FAILED = 1,
    USB_CAN_STATUS_BAD_PACKET = 2,
    USB_CAN_STATUS_BAD_DLC = 3,
    USB_CAN_STATUS_BAD_BITRATE = 4,
    USB_CAN_STATUS_CAN_INIT_FAILED = 5
};

typedef struct {
    uint8_t index;
    uint8_t buffer[USB_CAN_PACKET_SIZE];
} usb_packet_parser_t;

static const scp_gpio_assignment_t g_gpio_assignments[] = {
    {SIGNAL_HEARTBEAT_LED, 25},
    {SIGNAL_CAN_RX, 1},
    {SIGNAL_CAN_TX, 0},
};

const scp_module_config_t g_module_config = {
    .module_name = "usb_can_bridge",
    .module_id = SCP_MODULE_ID_USB_CAN_BRIDGE,
    .can_pio_num = 0,
    .can_bitrate = SCP_CAN_BITRATE,
    .gpio_assignments = g_gpio_assignments,
    .gpio_assignment_count = sizeof(g_gpio_assignments) / sizeof(g_gpio_assignments[0]),
};

static void write_u16_le(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t read_u32_le(const uint8_t data[4]) {
    return (uint32_t)data[0]
           | ((uint32_t)data[1] << 8)
           | ((uint32_t)data[2] << 16)
           | ((uint32_t)data[3] << 24);
}

static bool usb_send_packet(const uint8_t packet[USB_CAN_PACKET_SIZE]) {
    if (packet == NULL) {
        return false;
    }
    if (!stdio_usb_connected()) {
        return false;
    }

    for (size_t i = 0; i < USB_CAN_PACKET_SIZE; ++i) {
        putchar_raw((char)packet[i]);
    }
    return true;
}

static void usb_send_status(uint8_t status_code, uint32_t argument) {
    uint8_t packet[USB_CAN_PACKET_SIZE] = {0};
    packet[0] = USB_CAN_PACKET_MAGIC_0;
    packet[1] = USB_CAN_PACKET_MAGIC_1;
    packet[2] = USB_CAN_PACKET_TYPE_STATUS;
    packet[3] = status_code;
    write_u32_le(&packet[8], argument);
    (void)usb_send_packet(packet);
}

static void usb_send_can_rx(const struct can2040_msg *msg) {
    uint8_t packet[USB_CAN_PACKET_SIZE] = {0};

    if (msg == NULL) {
        return;
    }

    packet[0] = USB_CAN_PACKET_MAGIC_0;
    packet[1] = USB_CAN_PACKET_MAGIC_1;
    packet[2] = USB_CAN_PACKET_TYPE_CAN_RX;
    packet[3] = 0U;
    write_u16_le(&packet[4], (uint16_t)(msg->id & 0x7FFU));
    packet[6] = (uint8_t)(msg->dlc & 0x0FU);
    for (uint8_t i = 0; i < 8U; ++i) {
        packet[8U + i] = msg->data[i];
    }
    (void)usb_send_packet(packet);
}

static void usb_send_pong(const uint8_t request[USB_CAN_PACKET_SIZE]) {
    uint8_t packet[USB_CAN_PACKET_SIZE] = {0};

    if (request == NULL) {
        return;
    }

    packet[0] = USB_CAN_PACKET_MAGIC_0;
    packet[1] = USB_CAN_PACKET_MAGIC_1;
    packet[2] = USB_CAN_PACKET_TYPE_PONG;
    packet[3] = request[3];
    for (uint8_t i = 0; i < 8U; ++i) {
        packet[8U + i] = request[8U + i];
    }
    (void)usb_send_packet(packet);
}

static bool usb_packet_parser_push(usb_packet_parser_t *parser,
                                   uint8_t byte,
                                   uint8_t out_packet[USB_CAN_PACKET_SIZE]) {
    if (parser == NULL || out_packet == NULL) {
        return false;
    }

    if (parser->index == 0U) {
        if (byte != USB_CAN_PACKET_MAGIC_0) {
            return false;
        }
        parser->buffer[parser->index++] = byte;
        return false;
    }

    if (parser->index == 1U) {
        if (byte != USB_CAN_PACKET_MAGIC_1) {
            parser->index = (byte == USB_CAN_PACKET_MAGIC_0) ? 1U : 0U;
            if (parser->index == 1U) {
                parser->buffer[0] = USB_CAN_PACKET_MAGIC_0;
            }
            return false;
        }
        parser->buffer[parser->index++] = byte;
        return false;
    }

    parser->buffer[parser->index++] = byte;
    if (parser->index < USB_CAN_PACKET_SIZE) {
        return false;
    }

    for (uint8_t i = 0; i < USB_CAN_PACKET_SIZE; ++i) {
        out_packet[i] = parser->buffer[i];
    }
    parser->index = 0U;
    return true;
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

static bool should_forward_can_rx_to_usb(const struct can2040_msg *msg) {
    if (msg == NULL) {
        return false;
    }

    /*
     * Drop flash control/data echo traffic (0x300..0x37F). During flashing this can
     * flood the USB side and delay/drop target status frames. Keep status frames.
     */
    if (msg->id >= SCP_MSG_FLASH_CONTROL_BASE && msg->id < SCP_MSG_FLASH_STATUS_BASE) {
        return false;
    }

    return true;
}

static void handle_usb_packet(const uint8_t packet[USB_CAN_PACKET_SIZE],
                              scp_can_bus_t *can_bus,
                              uint8_t can_gpio_rx,
                              uint8_t can_gpio_tx) {
    if (packet == NULL || can_bus == NULL) {
        return;
    }

    switch (packet[2]) {
        case USB_CAN_PACKET_TYPE_CAN_TX: {
            struct can2040_msg tx_msg = {0};
            tx_msg.id = (uint32_t)(packet[4] | ((uint16_t)packet[5] << 8)) & 0x7FFU;
            tx_msg.dlc = packet[6] & 0x0FU;
            if (tx_msg.dlc > 8U) {
                usb_send_status(USB_CAN_STATUS_BAD_DLC, tx_msg.dlc);
                return;
            }
            for (uint8_t i = 0; i < 8U; ++i) {
                tx_msg.data[i] = packet[8U + i];
            }

            if (!can_transmit_with_retry(can_bus, &tx_msg, USB_CAN_TX_RETRY_TIMEOUT_US)) {
                usb_send_status(USB_CAN_STATUS_TX_FAILED, tx_msg.id);
            }
            return;
        }
        case USB_CAN_PACKET_TYPE_PING:
            usb_send_pong(packet);
            return;
        case USB_CAN_PACKET_TYPE_SET_BITRATE: {
            const uint32_t requested_bitrate = read_u32_le(&packet[8]);
            if (requested_bitrate < 125000U || requested_bitrate > 1000000U) {
                usb_send_status(USB_CAN_STATUS_BAD_BITRATE, requested_bitrate);
                return;
            }

            if (!scp_can_init(can_bus,
                              g_module_config.can_pio_num,
                              requested_bitrate,
                              can_gpio_rx,
                              can_gpio_tx)) {
                usb_send_status(USB_CAN_STATUS_CAN_INIT_FAILED, requested_bitrate);
                return;
            }

            usb_send_status(USB_CAN_STATUS_OK, requested_bitrate);
            return;
        }
        default:
            usb_send_status(USB_CAN_STATUS_BAD_PACKET, packet[2]);
            return;
    }
}

int main(void) {
    stdio_init_all();
    (void)scp_bootloader_run_if_requested(&g_module_config, SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS);

    scp_can_bus_t can_bus;
    scp_flash_can_target_t flash_target;
    scp_pico_gpio_map_t gpio_map;
    struct can2040_msg rx_msg;
    struct can2040_msg heartbeat_msg;
    usb_packet_parser_t parser = {0};
    uint8_t usb_packet[USB_CAN_PACKET_SIZE];
    uint8_t can_gpio_rx;
    uint8_t can_gpio_tx;
    uint8_t heartbeat_led_gpio;
    absolute_time_t next_heartbeat = nil_time;
    absolute_time_t next_led_flash = nil_time;
    uint8_t heartbeat_counter = 0U;

    if (!scp_module_build_gpio_map(&g_module_config, &gpio_map)) {
        return 1;
    }

    if (0
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)) {
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

    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    next_led_flash = make_timeout_time_ms(1500);

    while (true) {
        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);
        uint8_t usb_bytes_processed = 0U;
        uint8_t can_frames_forwarded = 0U;

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&heartbeat_msg, g_module_config.module_id, heartbeat_counter++, now_ms);
            (void)scp_can_transmit(&can_bus, &heartbeat_msg);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        if (absolute_time_diff_us(now, next_led_flash) <= 0) {
            gpio_put(heartbeat_led_gpio, 1);
            sleep_us(SCP_LED_FLASH_PULSE_US);
            gpio_put(heartbeat_led_gpio, 0);
            next_led_flash = make_timeout_time_ms(1500);
        }

        int usb_byte = getchar_timeout_us(0);
        while (usb_byte != PICO_ERROR_TIMEOUT && usb_bytes_processed < USB_CAN_MAX_USB_BYTES_PER_LOOP) {
            if (usb_packet_parser_push(&parser, (uint8_t)usb_byte, usb_packet)) {
                handle_usb_packet(usb_packet, &can_bus, can_gpio_rx, can_gpio_tx);
            }
            usb_bytes_processed++;
            usb_byte = getchar_timeout_us(0);
        }

        while (can_frames_forwarded < USB_CAN_MAX_CAN_RX_FORWARD_PER_LOOP && scp_can_try_read(&can_bus, &rx_msg)) {
            if (scp_flash_can_target_handle_can_frame(&flash_target, &can_bus, &rx_msg)) {
                continue;
            }
            if (should_forward_can_rx_to_usb(&rx_msg)) {
                usb_send_can_rx(&rx_msg);
                can_frames_forwarded++;
            }
        }

        sleep_us(20);
    }
}
