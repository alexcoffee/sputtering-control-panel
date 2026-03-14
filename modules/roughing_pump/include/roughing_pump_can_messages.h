#ifndef ROUGHING_PUMP_CAN_MESSAGES_H
#define ROUGHING_PUMP_CAN_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

struct can2040_msg;

void roughing_pump_build_switch_event(struct can2040_msg *msg, bool switch_on, uint32_t uptime_ms);

#endif
