#include "scp/can_messages.h"

#include <string.h>

#include "scp/can_bus.h"
#include "scp/protocol.h"

static void write_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

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

void build_pressure_reading_event(struct can2040_msg *msg, uint8_t module_id, float pressure_torr, bool connection_ok) {
    uint32_t pressure_bits = 0U;
    memcpy(&pressure_bits, &pressure_torr, sizeof(pressure_bits));

    msg->id = scp_protocol_event_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_EVENT_PRESSURE_READING;
    msg->data[3] = connection_ok ? 1U : 0U;
    msg->data[4] = (uint8_t) (pressure_bits & 0xFFU);
    msg->data[5] = (uint8_t) ((pressure_bits >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((pressure_bits >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((pressure_bits >> 24) & 0xFFU);
}

void build_power_reading_event(struct can2040_msg *msg, uint8_t module_id, float power_watts, bool connection_ok) {
    uint32_t power_bits = 0U;
    memcpy(&power_bits, &power_watts, sizeof(power_bits));

    msg->id = scp_protocol_event_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = SCP_EVENT_POWER_READING;
    msg->data[3] = connection_ok ? 1U : 0U;
    msg->data[4] = (uint8_t) (power_bits & 0xFFU);
    msg->data[5] = (uint8_t) ((power_bits >> 8) & 0xFFU);
    msg->data[6] = (uint8_t) ((power_bits >> 16) & 0xFFU);
    msg->data[7] = (uint8_t) ((power_bits >> 24) & 0xFFU);
}

void build_set_display_unit_command(struct can2040_msg *msg,
                                    uint8_t source_module_id,
                                    uint8_t target_module_id,
                                    uint8_t display_unit) {
    msg->id = scp_protocol_command_msg_id(target_module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = source_module_id;
    msg->data[2] = SCP_COMMAND_SET_DISPLAY_UNIT;
    msg->data[3] = display_unit;
    msg->data[4] = 0U;
    msg->data[5] = 0U;
    msg->data[6] = 0U;
    msg->data[7] = 0U;
}

void build_set_switch_command(struct can2040_msg *msg,
                              uint8_t source_module_id,
                              uint8_t target_module_id,
                              bool enabled) {
    msg->id = scp_protocol_command_msg_id(target_module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = source_module_id;
    msg->data[2] = SCP_COMMAND_SET_SWITCH;
    msg->data[3] = enabled ? 1U : 0U;
    msg->data[4] = 0U;
    msg->data[5] = 0U;
    msg->data[6] = 0U;
    msg->data[7] = 0U;
}

void build_flash_control_command(struct can2040_msg *msg,
                                 uint8_t source_module_id,
                                 uint8_t target_module_id,
                                 uint8_t command,
                                 uint8_t session_id,
                                 uint32_t argument) {
    if (msg == NULL) {
        return;
    }

    msg->id = scp_protocol_flash_control_msg_id(target_module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = source_module_id;
    msg->data[2] = command;
    msg->data[3] = session_id;
    write_u32_le(&msg->data[4], argument);
}

void build_flash_data_frame(struct can2040_msg *msg,
                            uint8_t target_module_id,
                            uint8_t session_id,
                            uint8_t sequence,
                            const uint8_t payload[SCP_FLASH_DATA_BYTES_PER_FRAME]) {
    if (msg == NULL) {
        return;
    }

    msg->id = scp_protocol_flash_data_msg_id(target_module_id);
    msg->dlc = 8;
    msg->data[0] = session_id;
    msg->data[1] = sequence;
    if (payload == NULL) {
        memset(&msg->data[2], 0, SCP_FLASH_DATA_BYTES_PER_FRAME);
    } else {
        memcpy(&msg->data[2], payload, SCP_FLASH_DATA_BYTES_PER_FRAME);
    }
}

void build_flash_status_event(struct can2040_msg *msg,
                              uint8_t module_id,
                              uint8_t status,
                              uint8_t session_id,
                              uint32_t argument) {
    if (msg == NULL) {
        return;
    }

    msg->id = scp_protocol_flash_status_msg_id(module_id);
    msg->dlc = 8;
    msg->data[0] = SCP_PROTOCOL_VERSION;
    msg->data[1] = module_id;
    msg->data[2] = status;
    msg->data[3] = session_id;
    write_u32_le(&msg->data[4], argument);
}
