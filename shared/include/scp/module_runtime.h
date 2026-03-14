#ifndef SCP_MODULE_RUNTIME_H
#define SCP_MODULE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "scp/pico_gpio_map.h"

typedef struct {
    const char *module_name;
    uint8_t module_id;
    uint8_t can_pio_num;
    uint32_t can_bitrate;
    const scp_gpio_assignment_t *gpio_assignments;
    size_t gpio_assignment_count;
} scp_module_config_t;

#define SWITCH_DEBOUNCE_US 20000
#define SCP_LED_FLASH_PULSE_US 300
#define SCP_HEARTBEAT_PERIOD 1500
#define ONLINE_MESSAGE_DELAY_MS 5000

int scp_module_run(const scp_module_config_t *cfg);
bool scp_module_build_gpio_map(const scp_module_config_t *cfg, scp_pico_gpio_map_t *gpio_map);

#endif
