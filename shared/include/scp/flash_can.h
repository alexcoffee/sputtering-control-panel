#ifndef SCP_FLASH_CAN_H
#define SCP_FLASH_CAN_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/flash.h"

#include "scp/can_bus.h"

#define SCP_FLASH_STAGING_SIZE_BYTES (768U * 1024U)
#define SCP_FLASH_RESERVED_TAIL_BYTES (64U * 1024U)

typedef struct {
    uint8_t module_id;
    uint8_t session_id;
    uint8_t ready_session_id;
    uint8_t expected_sequence;
    uint8_t progress_frame_counter;
    bool session_active;
    bool image_ready;
    uint32_t staging_offset;
    uint32_t staging_size;
    uint32_t image_size_bytes;
    uint32_t bytes_received;
    uint32_t bytes_programmed;
    uint32_t staged_page_offset;
    uint32_t crc32_state;
    uint16_t pending_write_jobs;
    uint8_t page_buffer[FLASH_PAGE_SIZE];
    uint16_t page_fill;
    bool worker_error;
} scp_flash_can_target_t;

void scp_flash_can_target_init(scp_flash_can_target_t *target, uint8_t module_id);
bool scp_flash_can_target_handle_can_frame(scp_flash_can_target_t *target,
                                           scp_can_bus_t *can_bus,
                                           const struct can2040_msg *msg);

#endif
