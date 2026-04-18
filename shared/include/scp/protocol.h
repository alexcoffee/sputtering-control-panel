#ifndef SCP_PROTOCOL_H
#define SCP_PROTOCOL_H

#include <stdint.h>

#define SCP_PROTOCOL_VERSION 1U
#define SCP_CAN_BITRATE_DEFAULT 500000U

enum {
    SCP_MSG_HEARTBEAT_BASE = 0x100,
    SCP_MSG_EVENT_BASE = 0x180,
    SCP_MSG_COMMAND_BASE = 0x200,
    SCP_MSG_FAULT_BASE = 0x080
};

enum {
    SCP_EVENT_SWITCH_CHANGED = 1,
    SCP_EVENT_CONNECTION_DETECTED = 2,
    SCP_EVENT_CONNECTION_LOST = 3,
    SCP_EVENT_PRESSURE_READING = 4,
    SCP_EVENT_CURRENT_READING = 5
};

enum {
    SCP_COMMAND_SET_DISPLAY_UNIT = 1
};

enum {
    SCP_DISPLAY_UNIT_TORR = 0,
    SCP_DISPLAY_UNIT_BAR = 1,
    SCP_DISPLAY_UNIT_VOLTAGE = 2
};

typedef enum {
    SCP_STATE_INIT = 0,
    SCP_STATE_READY = 1,
    SCP_STATE_RUN = 2,
    SCP_STATE_FAULT = 3,
    SCP_STATE_SAFE = 4
} scp_module_state_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t module_id;
    uint8_t state;
    uint8_t heartbeat_counter;
    uint32_t uptime_ms;
} scp_heartbeat_t;

uint32_t scp_protocol_heartbeat_msg_id(uint8_t module_id);
uint32_t scp_protocol_event_msg_id(uint8_t module_id);
uint32_t scp_protocol_command_msg_id(uint8_t module_id);

#endif
