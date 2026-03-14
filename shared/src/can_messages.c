#include "scp/can_messages.h"

#include "scp/can_bus.h"
#include "scp/protocol.h"

void build_heartbeat(struct can2040_msg *msg, uint8_t module_id, uint8_t counter, uint32_t uptime_ms) {
    msg->id = scp_protocol_heartbeat_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_STATE_RUN;
    msg->data[3] = counter;
    msg->data[4] = (uint8_t) (uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t) ((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((uptime_ms >> 24) & 0xFFU);
}

void build_connection_detected_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms) {
    msg->id = scp_protocol_event_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_EVENT_CONNECTION_DETECTED;
    msg->data[3] = 1U;
    msg->data[4] = (uint8_t) (uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t) ((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((uptime_ms >> 24) & 0xFFU);
}

void build_connection_lost_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms) {
    msg->id = scp_protocol_event_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_EVENT_CONNECTION_LOST;
    msg->data[3] = 0U;
    msg->data[4] = (uint8_t) (uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t) ((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((uptime_ms >> 24) & 0xFFU);
}
