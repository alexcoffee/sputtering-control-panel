#include "scp/bootloader.h"

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "scp/can_messages.h"
#include "scp/can_bus.h"
#include "scp/flash_can.h"

#define SCP_BOOTLOADER_MAGIC_BASE 0x5343424Du
#define SCP_BOOTLOADER_SCRATCH_INDEX 7U
#define SCP_BOOTLOADER_LED_FLASH_PERIOD_MS 1400U

static uint32_t scp_bootloader_magic_for_module(uint8_t module_id) {
    return SCP_BOOTLOADER_MAGIC_BASE ^ ((uint32_t)module_id << 8) ^ 0xA5u;
}

void scp_bootloader_request(uint8_t module_id) {
    watchdog_hw->scratch[SCP_BOOTLOADER_SCRATCH_INDEX] = scp_bootloader_magic_for_module(module_id);
}

void scp_bootloader_clear(void) {
    watchdog_hw->scratch[SCP_BOOTLOADER_SCRATCH_INDEX] = 0u;
}

bool scp_bootloader_requested(uint8_t module_id) {
    return watchdog_hw->scratch[SCP_BOOTLOADER_SCRATCH_INDEX] == scp_bootloader_magic_for_module(module_id);
}

bool scp_bootloader_run_if_requested(const scp_module_config_t *cfg, uint32_t idle_timeout_ms) {
    scp_pico_gpio_map_t gpio_map;
    scp_can_bus_t can_bus;
    scp_flash_can_target_t flash_target;
    struct can2040_msg rx_msg;
    struct can2040_msg heartbeat_msg;
    uint8_t can_gpio_rx = 0U;
    uint8_t can_gpio_tx = 0U;
    uint8_t heartbeat_led_gpio = 0U;
    bool has_heartbeat_led = false;
    uint8_t heartbeat_counter = 0U;
    absolute_time_t next_heartbeat = nil_time;
    absolute_time_t next_led_flash = nil_time;
    absolute_time_t idle_deadline;
    const uint32_t timeout_ms =
        (idle_timeout_ms == 0U) ? SCP_BOOTLOADER_DEFAULT_IDLE_TIMEOUT_MS : idle_timeout_ms;

    if (cfg == NULL) {
        return false;
    }
    if (!scp_bootloader_requested(cfg->module_id)) {
        return false;
    }

    scp_bootloader_clear();

    if (!scp_module_build_gpio_map(cfg, &gpio_map)) {
        return true;
    }
    if (!scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_RX, &can_gpio_rx)
        || !scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_CAN_TX, &can_gpio_tx)) {
        return true;
    }

    has_heartbeat_led = scp_pico_gpio_map_find_pin(&gpio_map, SIGNAL_HEARTBEAT_LED, &heartbeat_led_gpio);
    if (has_heartbeat_led) {
        gpio_init(heartbeat_led_gpio);
        gpio_set_dir(heartbeat_led_gpio, GPIO_OUT);
        gpio_put(heartbeat_led_gpio, 0);
    }

    if (!scp_can_init(&can_bus, cfg->can_pio_num, cfg->can_bitrate, can_gpio_rx, can_gpio_tx)) {
        return true;
    }

    scp_flash_can_target_init(&flash_target, cfg->module_id);
    next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
    next_led_flash = make_timeout_time_ms(SCP_BOOTLOADER_LED_FLASH_PERIOD_MS);
    idle_deadline = make_timeout_time_ms(timeout_ms);

    while (true) {
        bool flash_active = flash_target.session_active || flash_target.image_ready;

        while (scp_can_try_read(&can_bus, &rx_msg)) {
            if (scp_flash_can_target_handle_can_frame(&flash_target, &can_bus, &rx_msg)) {
                flash_active = flash_target.session_active || flash_target.image_ready;
                idle_deadline = make_timeout_time_ms(timeout_ms);
            }
        }

        if (flash_active) {
            /* Keep the flash-update path minimal while page programs are in flight. */
            tight_loop_contents();
            continue;
        }

        const absolute_time_t now = get_absolute_time();
        const uint32_t now_ms = to_ms_since_boot(now);

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            build_heartbeat(&heartbeat_msg, cfg->module_id, heartbeat_counter++, now_ms);
            (void)scp_can_transmit(&can_bus, &heartbeat_msg);
            next_heartbeat = make_timeout_time_ms(SCP_HEARTBEAT_PERIOD);
        }

        if (has_heartbeat_led && absolute_time_diff_us(now, next_led_flash) <= 0) {
            gpio_put(heartbeat_led_gpio, 1);
            sleep_us(SCP_LED_FLASH_PULSE_US);
            gpio_put(heartbeat_led_gpio, 0);
            next_led_flash = make_timeout_time_ms(SCP_BOOTLOADER_LED_FLASH_PERIOD_MS);
        }

        if (time_reached(idle_deadline)) {
            break;
        }

        sleep_us(20);
    }

    return true;
}
