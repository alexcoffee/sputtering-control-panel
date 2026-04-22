#include "scp/flash_can.h"

#include <string.h>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "scp/bootloader.h"
#include "scp/can_messages.h"
#include "scp/protocol.h"

static uint8_t g_flash_copy_sector_buffer[FLASH_SECTOR_SIZE];

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
    if (alignment == 0U) {
        return value;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t read_u32_le(const uint8_t data[4]) {
    return (uint32_t)data[0]
           | ((uint32_t)data[1] << 8)
           | ((uint32_t)data[2] << 16)
           | ((uint32_t)data[3] << 24);
}

static uint32_t crc32_update_byte(uint32_t crc, uint8_t byte) {
    crc ^= (uint32_t)byte;
    for (uint8_t i = 0; i < 8U; ++i) {
        const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
        crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
    return crc;
}

static bool flash_program_page_absolute(uint32_t flash_offset, const uint8_t page[FLASH_PAGE_SIZE]) {
    if (page == NULL) {
        return false;
    }

    const uint32_t irq_state = save_and_disable_interrupts();
    flash_range_program(flash_offset, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);
    return true;
}

static bool flash_program_page_for_target(scp_flash_can_target_t *target,
                                          uint32_t flash_offset,
                                          uint16_t data_bytes,
                                          const uint8_t page_data[FLASH_PAGE_SIZE]) {
    if (target == NULL || page_data == NULL) {
        return false;
    }
    if (!flash_program_page_absolute(flash_offset, page_data)) {
        target->worker_error = true;
        return false;
    }

    uint32_t bytes_programmed = target->bytes_programmed + (uint32_t)data_bytes;
    if (bytes_programmed > target->image_size_bytes) {
        bytes_programmed = target->image_size_bytes;
    }
    target->bytes_programmed = bytes_programmed;
    return true;
}

static void flash_session_reset(scp_flash_can_target_t *target) {
    if (target == NULL) {
        return;
    }
    target->session_id = 0U;
    target->expected_sequence = 0U;
    target->progress_frame_counter = 0U;
    target->session_active = false;
    target->image_size_bytes = 0U;
    target->bytes_received = 0U;
    target->bytes_programmed = 0U;
    target->staged_page_offset = 0U;
    target->crc32_state = 0xFFFFFFFFU;
    target->pending_write_jobs = 0U;
    target->page_fill = 0U;
    target->worker_error = false;
    memset(target->page_buffer, 0xFF, sizeof(target->page_buffer));
}

static void flash_send_status(scp_flash_can_target_t *target,
                              scp_can_bus_t *can_bus,
                              uint8_t status,
                              uint8_t session_id,
                              uint32_t argument) {
    struct can2040_msg tx_msg;

    if (target == NULL || can_bus == NULL) {
        return;
    }

    build_flash_status_event(&tx_msg, target->module_id, status, session_id, argument);
    (void)scp_can_transmit(can_bus, &tx_msg);
}

static void flash_send_error(scp_flash_can_target_t *target,
                             scp_can_bus_t *can_bus,
                             uint8_t session_id,
                             uint8_t error_code) {
    flash_send_status(target, can_bus, SCP_FLASH_STATUS_ERROR, session_id, (uint32_t)error_code);
}

static bool flash_erase_staging_area(const scp_flash_can_target_t *target, uint32_t image_size_bytes) {
    const uint32_t erase_bytes = align_up_u32(image_size_bytes, FLASH_SECTOR_SIZE);

    if (target == NULL || erase_bytes == 0U || erase_bytes > target->staging_size) {
        return false;
    }

    for (uint32_t offset = 0U; offset < erase_bytes; offset += FLASH_SECTOR_SIZE) {
        const uint32_t irq_state = save_and_disable_interrupts();
        flash_range_erase(target->staging_offset + offset, FLASH_SECTOR_SIZE);
        restore_interrupts(irq_state);
    }

    return true;
}

static bool flash_stage_data(scp_flash_can_target_t *target, const uint8_t *data, uint32_t length) {
    if (target == NULL || data == NULL) {
        return false;
    }
    if (target->worker_error) {
        return false;
    }

    for (uint32_t i = 0U; i < length; ++i) {
        if (target->bytes_received >= target->image_size_bytes) {
            break;
        }

        if (target->page_fill == 0U) {
            memset(target->page_buffer, 0xFF, sizeof(target->page_buffer));
        }

        target->page_buffer[target->page_fill++] = data[i];
        target->crc32_state = crc32_update_byte(target->crc32_state, data[i]);
        target->bytes_received++;

        if (target->page_fill == FLASH_PAGE_SIZE || target->bytes_received == target->image_size_bytes) {
            const uint32_t page_offset = target->staged_page_offset;
            const uint32_t flash_offset = target->staging_offset + page_offset;
            const uint16_t valid_bytes = (target->page_fill == FLASH_PAGE_SIZE)
                                         ? FLASH_PAGE_SIZE
                                         : target->page_fill;

            if (page_offset > target->staging_size || page_offset > UINT32_MAX - target->staging_offset) {
                return false;
            }
            if (flash_offset + FLASH_PAGE_SIZE > target->staging_offset + target->staging_size) {
                return false;
            }
            if (!flash_program_page_for_target(target, flash_offset, valid_bytes, target->page_buffer)) {
                return false;
            }
            target->staged_page_offset += FLASH_PAGE_SIZE;
            target->page_fill = 0U;
        }
    }

    return true;
}

static void __no_inline_not_in_flash_func(scp_flash_apply_staged_image)(uint32_t staging_offset, uint32_t image_size_bytes) {
    uint32_t copy_bytes = image_size_bytes;
    if ((copy_bytes % FLASH_SECTOR_SIZE) != 0U) {
        copy_bytes += FLASH_SECTOR_SIZE - (copy_bytes % FLASH_SECTOR_SIZE);
    }
    const uint32_t irq_state = save_and_disable_interrupts();

    for (uint32_t target_offset = 0U; target_offset < copy_bytes; target_offset += FLASH_SECTOR_SIZE) {
        const uint8_t *source_sector = (const uint8_t *)(XIP_BASE + staging_offset + target_offset);
        for (uint32_t i = 0U; i < FLASH_SECTOR_SIZE; ++i) {
            g_flash_copy_sector_buffer[i] = source_sector[i];
        }

        flash_range_erase(target_offset, FLASH_SECTOR_SIZE);
        for (uint32_t page = 0U; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
            flash_range_program(target_offset + page, &g_flash_copy_sector_buffer[page], FLASH_PAGE_SIZE);
        }
    }

    restore_interrupts(irq_state);
    watchdog_reboot(0U, 0U, 0U);
    while (true) {
        tight_loop_contents();
    }
}

static bool flash_is_control_frame_for_target(const struct can2040_msg *msg, uint8_t module_id) {
    if (msg == NULL) {
        return false;
    }

    return msg->id == scp_protocol_flash_control_msg_id(module_id)
           && msg->dlc == 8U
           && msg->data[0] == SCP_PROTOCOL_VERSION;
}

static bool flash_is_data_frame_for_target(const struct can2040_msg *msg, uint8_t module_id) {
    if (msg == NULL) {
        return false;
    }

    return msg->id == scp_protocol_flash_data_msg_id(module_id) && msg->dlc == 8U;
}

void scp_flash_can_target_init(scp_flash_can_target_t *target, uint8_t module_id) {
    if (target == NULL) {
        return;
    }

    memset(target, 0, sizeof(*target));
    target->module_id = module_id;
    target->crc32_state = 0xFFFFFFFFU;

    if (PICO_FLASH_SIZE_BYTES <= SCP_FLASH_RESERVED_TAIL_BYTES + SCP_FLASH_STAGING_SIZE_BYTES) {
        target->staging_offset = 0U;
        target->staging_size = 0U;
        return;
    }

    target->staging_size = align_up_u32(SCP_FLASH_STAGING_SIZE_BYTES, FLASH_SECTOR_SIZE);
    target->staging_offset = PICO_FLASH_SIZE_BYTES - SCP_FLASH_RESERVED_TAIL_BYTES - target->staging_size;
    target->staging_offset &= ~(FLASH_SECTOR_SIZE - 1U);
    memset(target->page_buffer, 0xFF, sizeof(target->page_buffer));
}

bool scp_flash_can_target_handle_can_frame(scp_flash_can_target_t *target,
                                           scp_can_bus_t *can_bus,
                                           const struct can2040_msg *msg) {
    const uint8_t session_id = (msg != NULL) ? msg->data[3] : 0U;

    if (target == NULL || can_bus == NULL || msg == NULL) {
        return false;
    }

    if (flash_is_control_frame_for_target(msg, target->module_id)) {
        const uint8_t command = msg->data[2];
        const uint32_t argument = read_u32_le(&msg->data[4]);

        switch (command) {
            case SCP_FLASH_COMMAND_BEGIN: {
                if (target->staging_size == 0U || argument == 0U) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_UNSUPPORTED);
                    return true;
                }
                if (argument > target->staging_size || argument > target->staging_offset) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_IMAGE_TOO_LARGE);
                    return true;
                }

                target->image_ready = false;
                flash_session_reset(target);
                if (!flash_erase_staging_area(target, argument)) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_FLASH_WRITE_FAILED);
                    return true;
                }

                target->session_active = true;
                target->session_id = session_id;
                target->image_size_bytes = argument;
                target->crc32_state = 0xFFFFFFFFU;
                flash_send_status(target, can_bus, SCP_FLASH_STATUS_ACK, target->session_id, target->image_size_bytes);
                return true;
            }
            case SCP_FLASH_COMMAND_FINISH: {
                if (!target->session_active) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_STATE);
                    return true;
                }
                if (session_id != target->session_id) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_SESSION_MISMATCH);
                    return true;
                }
                if (target->bytes_received != target->image_size_bytes) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_IMAGE_INCOMPLETE);
                    flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
                    return true;
                }
                if (target->worker_error || target->bytes_programmed != target->image_size_bytes) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_FLASH_WRITE_FAILED);
                    return true;
                }

                const uint32_t crc32 = target->crc32_state ^ 0xFFFFFFFFU;
                if (crc32 != argument) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_CRC_MISMATCH);
                    return true;
                }

                target->image_ready = true;
                target->ready_session_id = session_id;
                target->session_active = false;
                flash_send_status(target, can_bus, SCP_FLASH_STATUS_READY, target->ready_session_id, target->image_size_bytes);
                return true;
            }
            case SCP_FLASH_COMMAND_COMMIT: {
                if (!target->image_ready || session_id != target->ready_session_id) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_STATE);
                    return true;
                }

                flash_send_status(target, can_bus, SCP_FLASH_STATUS_COMMITTING, target->ready_session_id, target->image_size_bytes);
                sleep_ms(10);
                scp_flash_apply_staged_image(target->staging_offset, target->image_size_bytes);
                flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_FLASH_WRITE_FAILED);
                return true;
            }
            case SCP_FLASH_COMMAND_ABORT: {
                if (target->session_active) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_STATE);
                    flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
                    return true;
                }
                flash_session_reset(target);
                target->image_ready = false;
                flash_send_status(target, can_bus, SCP_FLASH_STATUS_ACK, session_id, 0U);
                return true;
            }
            case SCP_FLASH_COMMAND_ENTER_BOOTSEL: {
                if (target->session_active) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_STATE);
                    flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
                    return true;
                }
                flash_send_status(target, can_bus, SCP_FLASH_STATUS_ACK, session_id, 0U);
                sleep_ms(10);
                reset_usb_boot(0U, 0U);
                return true;
            }
            case SCP_FLASH_COMMAND_REBOOT_TO_BOOTLOADER: {
                if (target->session_active) {
                    flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_STATE);
                    flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
                    return true;
                }
                flash_session_reset(target);
                target->image_ready = false;
                flash_send_status(target, can_bus, SCP_FLASH_STATUS_ACK, session_id, 0U);
                sleep_ms(10);
                scp_bootloader_request(target->module_id);
                watchdog_reboot(0U, 0U, 10U);
                while (true) {
                    tight_loop_contents();
                }
            }
            default:
                flash_send_error(target, can_bus, session_id, SCP_FLASH_ERROR_INVALID_ARGUMENT);
                return true;
        }
    }

    if (flash_is_data_frame_for_target(msg, target->module_id)) {
        const uint8_t data_session_id = msg->data[0];
        const uint8_t sequence = msg->data[1];

        if (!target->session_active) {
            flash_send_error(target, can_bus, data_session_id, SCP_FLASH_ERROR_INVALID_STATE);
            return true;
        }
        if (data_session_id != target->session_id) {
            flash_send_error(target, can_bus, data_session_id, SCP_FLASH_ERROR_SESSION_MISMATCH);
            return true;
        }
        if (target->worker_error) {
            flash_send_error(target, can_bus, data_session_id, SCP_FLASH_ERROR_FLASH_WRITE_FAILED);
            flash_session_reset(target);
            target->image_ready = false;
            return true;
        }
        if (sequence != target->expected_sequence) {
            flash_send_error(target, can_bus, data_session_id, SCP_FLASH_ERROR_BAD_SEQUENCE);
            flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
            return true;
        }

        if (!flash_stage_data(target, &msg->data[2], SCP_FLASH_DATA_BYTES_PER_FRAME)) {
            flash_send_error(target, can_bus, data_session_id, SCP_FLASH_ERROR_FLASH_WRITE_FAILED);
            flash_session_reset(target);
            target->image_ready = false;
            return true;
        }

        target->expected_sequence++;
        target->progress_frame_counter++;
        if (target->progress_frame_counter >= SCP_FLASH_PROGRESS_INTERVAL_FRAMES
            || target->bytes_received == target->image_size_bytes) {
            target->progress_frame_counter = 0U;
            flash_send_status(target, can_bus, SCP_FLASH_STATUS_PROGRESS, target->session_id, target->bytes_received);
        }
        return true;
    }

    return false;
}
