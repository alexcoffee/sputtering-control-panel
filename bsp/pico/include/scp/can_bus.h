#ifndef SCP_CAN_BUS_H
#define SCP_CAN_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "can2040.h"

typedef struct {
    struct can2040 can;
    volatile uint8_t rx_head;
    volatile uint8_t rx_tail;
    struct can2040_msg rx_queue[16];
    int pio_num;
} scp_can_bus_t;

bool scp_can_init(scp_can_bus_t *bus, int pio_num, uint32_t bitrate, int gpio_rx, int gpio_tx);
bool scp_can_transmit(scp_can_bus_t *bus, struct can2040_msg *msg);
bool scp_can_try_read(scp_can_bus_t *bus, struct can2040_msg *msg_out);

#endif
