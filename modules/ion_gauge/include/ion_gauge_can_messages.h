#ifndef ION_GAUGE_CAN_MESSAGES_H
#define ION_GAUGE_CAN_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

struct can2040_msg;

void ion_gauge_build_switch_event(struct can2040_msg *msg, bool switch_on, uint32_t uptime_ms);

#endif
