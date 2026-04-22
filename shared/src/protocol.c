#include "scp/protocol.h"

uint32_t scp_protocol_heartbeat_msg_id(uint8_t module_id) {
    return SCP_MSG_HEARTBEAT_BASE + (uint32_t)module_id;
}

uint32_t scp_protocol_event_msg_id(uint8_t module_id) {
    return SCP_MSG_EVENT_BASE + (uint32_t)module_id;
}

uint32_t scp_protocol_command_msg_id(uint8_t module_id) {
    return SCP_MSG_COMMAND_BASE + (uint32_t)module_id;
}

uint32_t scp_protocol_flash_control_msg_id(uint8_t module_id) {
    return SCP_MSG_FLASH_CONTROL_BASE + (uint32_t)module_id;
}

uint32_t scp_protocol_flash_data_msg_id(uint8_t module_id) {
    return SCP_MSG_FLASH_DATA_BASE + (uint32_t)module_id;
}

uint32_t scp_protocol_flash_status_msg_id(uint8_t module_id) {
    return SCP_MSG_FLASH_STATUS_BASE + (uint32_t)module_id;
}
