#ifndef SCP_BOOTLOADER_H
#define SCP_BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "scp/module_runtime.h"

#define SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS 12000U

void scp_bootloader_request(uint8_t module_id);
void scp_bootloader_clear(void);
bool scp_bootloader_requested(uint8_t module_id);
bool scp_bootloader_run_if_requested(const scp_module_config_t *cfg, uint32_t idle_timeout_ms);

#endif
