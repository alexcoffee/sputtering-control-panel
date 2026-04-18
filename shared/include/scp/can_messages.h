#ifndef SCP_CAN_MESSAGES_H
#define SCP_CAN_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

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

#endif
