#include "roughing_pump_can_messages.h"

#include "module_config.h"
#include "scp/can_bus.h"
#include "scp/protocol.h"

void roughing_pump_build_switch_event(struct can2040_msg *msg, bool switch_on, uint32_t uptime_ms) {
    msg->id = scp_protocol_event_msg_id(g_module_config.module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = g_module_config.module_id;
    msg->data[2] = SCP_EVENT_SWITCH_CHANGED;
    msg->data[3] = switch_on ? 1U : 0U;
    msg->data[4] = (uint8_t) (uptime_ms & 0xFFU);
    msg->data[5] = (uint8_t) ((uptime_ms >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((uptime_ms >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((uptime_ms >> 24) & 0xFFU);
}
