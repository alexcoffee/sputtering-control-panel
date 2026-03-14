#include "scp/can_bus.h"

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

static scp_can_bus_t *g_can_bus_instances[2] = {0};

static void scp_can_rx_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
    if (notify != CAN2040_NOTIFY_RX || cd == NULL || msg == NULL) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        scp_can_bus_t *bus = g_can_bus_instances[i];
        if (bus != NULL && &bus->can == cd) {
            bus->rx_msg = *msg;
            bus->rx_pending = true;
            return;
        }
    }
}

static void scp_can_pio0_irq(void) {
    scp_can_bus_t *bus = g_can_bus_instances[0];
    if (bus != NULL) {
        can2040_pio_irq_handler(&bus->can);
    }
}

static void scp_can_pio1_irq(void) {
    scp_can_bus_t *bus = g_can_bus_instances[1];
    if (bus != NULL) {
        can2040_pio_irq_handler(&bus->can);
    }
}

bool scp_can_init(scp_can_bus_t *bus, int pio_num, uint32_t bitrate, int gpio_rx, int gpio_tx) {
    uint32_t sys_clock_hz;

    if (bus == NULL || pio_num < 0 || pio_num > 1 || bitrate == 0) {
        return false;
    }

    bus->rx_pending = false;
    bus->pio_num = pio_num;
    sys_clock_hz = clock_get_hz(clk_sys);

    can2040_setup(&bus->can, pio_num);
    can2040_callback_config(&bus->can, scp_can_rx_cb);

    if (pio_num == 0) {
        g_can_bus_instances[0] = bus;
        irq_set_exclusive_handler(PIO0_IRQ_0, scp_can_pio0_irq);
        irq_set_priority(PIO0_IRQ_0, 1);
        irq_set_enabled(PIO0_IRQ_0, true);
    } else {
        g_can_bus_instances[1] = bus;
        irq_set_exclusive_handler(PIO1_IRQ_0, scp_can_pio1_irq);
        irq_set_priority(PIO1_IRQ_0, 1);
        irq_set_enabled(PIO1_IRQ_0, true);
    }

    can2040_start(&bus->can, sys_clock_hz, bitrate, gpio_rx, gpio_tx);
    return true;
}

bool scp_can_transmit(scp_can_bus_t *bus, struct can2040_msg *msg) {
    if (bus == NULL || msg == NULL) {
        return false;
    }
    return can2040_transmit(&bus->can, msg) == 0;
}

bool scp_can_try_read(scp_can_bus_t *bus, struct can2040_msg *msg_out) {
    uint32_t status;

    if (bus == NULL || msg_out == NULL || !bus->rx_pending) {
        return false;
    }

    status = save_and_disable_interrupts();
    *msg_out = bus->rx_msg;
    bus->rx_pending = false;
    restore_interrupts(status);
    return true;
}
