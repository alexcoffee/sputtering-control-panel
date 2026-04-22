#ifndef SCP_CAN_MESSAGES_H
#define SCP_CAN_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

#include "scp/protocol.h"

struct can2040_msg;

void build_heartbeat(struct can2040_msg *msg, uint8_t module_id, uint8_t counter, uint32_t uptime_ms);
void build_connection_detected_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms);
void build_connection_lost_event(struct can2040_msg *msg, uint8_t module_id, uint32_t uptime_ms);
void build_pressure_reading_event(struct can2040_msg *msg, uint8_t module_id, float pressure_torr, bool connection_ok);
void build_current_reading_event(struct can2040_msg *msg, uint8_t module_id, float current_amps, bool connection_ok);
void build_set_display_unit_command(struct can2040_msg *msg,
                                    uint8_t source_module_id,
                                    uint8_t target_module_id,
                                    uint8_t display_unit);
void build_flash_control_command(struct can2040_msg *msg,
                                 uint8_t source_module_id,
                                 uint8_t target_module_id,
                                 uint8_t command,
                                 uint8_t session_id,
                                 uint32_t argument);
void build_flash_data_frame(struct can2040_msg *msg,
                            uint8_t target_module_id,
                            uint8_t session_id,
                            uint8_t sequence,
                            const uint8_t payload[SCP_FLASH_DATA_BYTES_PER_FRAME]);
void build_flash_status_event(struct can2040_msg *msg,
                              uint8_t module_id,
                              uint8_t status,
                              uint8_t session_id,
                              uint32_t argument);

#endif
