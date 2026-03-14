#ifndef SCP_CAN_MESSAGES_H
#define SCP_CAN_MESSAGES_H

#include <stdint.h>

struct can2040_msg;

void build_heartbeat(struct can2040_msg *msg, uint8_t module_id, uint8_t counter, uint32_t uptime_ms);
void build_connection_detected_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms);
void build_connection_lost_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms);

#endif
