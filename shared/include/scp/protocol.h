#ifndef SCP_PROTOCOL_H
#define SCP_PROTOCOL_H

#include <stdint.h>

#define SCP_PROTOCOL_VERSION 1U
#define SCP_CAN_BITRATE 500000U

enum {
    SCP_MSG_HEARTBEAT_BASE = 0x100,
    SCP_MSG_EVENT_BASE = 0x180,
    SCP_MSG_COMMAND_BASE = 0x200,
    SCP_MSG_FAULT_BASE = 0x080,
    SCP_MSG_FLASH_CONTROL_BASE = 0x300,
    SCP_MSG_FLASH_DATA_BASE = 0x340,
    SCP_MSG_FLASH_STATUS_BASE = 0x380
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
    SCP_FLASH_COMMAND_BEGIN = 1,
    SCP_FLASH_COMMAND_FINISH = 2,
    SCP_FLASH_COMMAND_COMMIT = 3,
    SCP_FLASH_COMMAND_ABORT = 4,
    SCP_FLASH_COMMAND_ENTER_BOOTSEL = 5,
    SCP_FLASH_COMMAND_REBOOT_TO_BOOTLOADER = 6
};
/* Flash control frame (ID: SCP_MSG_FLASH_CONTROL_BASE + target module_id):
 *   data[0] = protocol version
 *   data[1] = source module_id
 *   data[2] = flash command
 *   data[3] = session_id
 *   data[4..7] = argument (u32, little-endian)
 *
 * Flash data frame (ID: SCP_MSG_FLASH_DATA_BASE + target module_id):
 *   data[0] = session_id
 *   data[1] = sequence
 *   data[2..7] = data payload bytes
 *
 * Flash status frame (ID: SCP_MSG_FLASH_STATUS_BASE + source module_id):
 *   data[0] = protocol version
 *   data[1] = source module_id
 *   data[2] = status
 *   data[3] = session_id
 *   data[4..7] = argument (u32, little-endian)
 */

enum {
    SCP_FLASH_STATUS_ACK = 1,
    SCP_FLASH_STATUS_PROGRESS = 2,
    SCP_FLASH_STATUS_READY = 3,
    SCP_FLASH_STATUS_COMMITTING = 4,
    SCP_FLASH_STATUS_DONE = 5,
    SCP_FLASH_STATUS_ERROR = 127
};

enum {
    SCP_FLASH_ERROR_NONE = 0,
    SCP_FLASH_ERROR_INVALID_STATE = 1,
    SCP_FLASH_ERROR_INVALID_ARGUMENT = 2,
    SCP_FLASH_ERROR_SESSION_MISMATCH = 3,
    SCP_FLASH_ERROR_BAD_SEQUENCE = 4,
    SCP_FLASH_ERROR_IMAGE_TOO_LARGE = 5,
    SCP_FLASH_ERROR_FLASH_WRITE_FAILED = 6,
    SCP_FLASH_ERROR_IMAGE_INCOMPLETE = 7,
    SCP_FLASH_ERROR_CRC_MISMATCH = 8,
    SCP_FLASH_ERROR_UNSUPPORTED = 9
};

#define SCP_FLASH_DATA_BYTES_PER_FRAME 6U
#define SCP_FLASH_PROGRESS_INTERVAL_FRAMES 16U

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
uint32_t scp_protocol_flash_control_msg_id(uint8_t module_id);
uint32_t scp_protocol_flash_data_msg_id(uint8_t module_id);
uint32_t scp_protocol_flash_status_msg_id(uint8_t module_id);

#endif
