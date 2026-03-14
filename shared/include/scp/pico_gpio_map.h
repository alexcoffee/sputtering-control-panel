#ifndef SCP_PICO_GPIO_MAP_H
#define SCP_PICO_GPIO_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCP_PICO_GPIO_MIN 0
#define SCP_PICO_GPIO_MAX 29
#define SCP_PICO_GPIO_COUNT (SCP_PICO_GPIO_MAX + 1)


// Shared GPIO
#define SCP_GPIO_SIGNAL_HEARTBEAT_LED "LED"

#define SCP_GPIO_SIGNAL_CAN_RX "CAN_RX"
#define SCP_GPIO_SIGNAL_CAN_TX "CAN_TX"

#define SCP_GPIO_SIGNAL_CONNECTION_OK "CONNECTION_OK"
#define SCP_GPIO_SIGNAL_CONNECTION_ACTIVITY "CONNECTION_ACTIVITY"
#define SCP_GPIO_SIGNAL_CONNECTION_DETECT "CONNECTION_DETECT"

#define SCP_GPIO_SIGNAL_SSR "SSR"
#define SCP_GPIO_SIGNAL_SWITCH_A "SWITCH_A"
#define SCP_GPIO_SIGNAL_SWITCH_B "SWITCH_B"

#define SCP_GPIO_SIGNAL_LCD_SPI_SCK "LCD_SPI_SCK"
#define SCP_GPIO_SIGNAL_LCD_SPI_TX "LCD_SPI_TX"
#define SCP_GPIO_SIGNAL_LCD_SPI_CSN "LCD_SPI_CSN"
#define SCP_GPIO_SIGNAL_LCD_COMMAND "LCD_COMMAND"
#define SCP_GPIO_SIGNAL_LCD_RESET "LCD_RESET"
#define SCP_GPIO_SIGNAL_LCD_BACKLIGHT "LCD_BACKLIGHT"
#define SCP_GPIO_SIGNAL_PRESSURE_SENSOR_ADC "PRESSURE_SENSOR_ADC"

typedef struct {
    const char *signal_name;
    uint8_t gpio;
} scp_gpio_assignment_t;

typedef struct {
    const char *signal_by_gpio[SCP_PICO_GPIO_COUNT];
} scp_pico_gpio_map_t;

bool scp_pico_gpio_map_build(const scp_gpio_assignment_t *assignments,
                             size_t assignment_count,
                             scp_pico_gpio_map_t *out_map,
                             char *error_buf,
                             size_t error_buf_size);

bool scp_pico_gpio_map_find_pin(const scp_pico_gpio_map_t *map, const char *signal_name, uint8_t *out_gpio);

#endif
