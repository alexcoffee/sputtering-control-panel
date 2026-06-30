#ifndef SCP_PICO_GPIO_MAP_H
#define SCP_PICO_GPIO_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCP_PICO_GPIO_MIN 0
#define SCP_PICO_GPIO_MAX 29
#define SCP_PICO_GPIO_COUNT (SCP_PICO_GPIO_MAX + 1)


// Shared GPIO
#define SIGNAL_HEARTBEAT_LED "LED"

#define SIGNAL_CAN_RX "CAN_RX"
#define SIGNAL_CAN_TX "CAN_TX"

#define SIGNAL_HOST_SPI_SCK "HOST_SPI_SCK"
#define SIGNAL_HOST_SPI_TX "HOST_SPI_TX"
#define SIGNAL_HOST_SPI_RX "HOST_SPI_RX"
#define SIGNAL_HOST_SPI_CSN "HOST_SPI_CSN"

#define SIGNAL_CONNECTION_OK "CONNECTION_OK"
#define SIGNAL_CONNECTION_ACTIVITY "CONNECTION_ACTIVITY"
#define SIGNAL_CONNECTION_DETECT "CONNECTION_DETECT"

#define SIGNAL_SSR "SSR"
#define SIGNAL_SWITCH_A "SWITCH_A"
#define SIGNAL_SWITCH_B "SWITCH_B"
#define SIGNAL_SWITCH_ENABLE "SWITCH_ENABLE"
#define SIGNAL_SWITCH_DEGAS "SWITCH_DEGAS"

#define SIGNAL_LCD_SPI_SCK "LCD_SPI_SCK"
#define SIGNAL_LCD_SDI "LCD_SDI"
#define SIGNAL_LCD_SDO "LCD_SDO"
#define SIGNAL_LCD_SPI_TX SIGNAL_LCD_SDI
#define SIGNAL_LCD_SPI_RX SIGNAL_LCD_SDO
#define SIGNAL_LCD_SPI_CSN "LCD_SPI_CSN"
#define SIGNAL_LCD_COMMAND "LCD_COMMAND"
#define SIGNAL_LCD_RESET "LCD_RESET"
#define SIGNAL_LCD_BACKLIGHT "LCD_BACKLIGHT"
#define SIGNAL_TOUCH_SPI_SCK "TOUCH_SPI_SCK"
#define SIGNAL_TOUCH_SDI "TOUCH_SDI"
#define SIGNAL_TOUCH_SDO "TOUCH_SDO"
#define SIGNAL_TOUCH_SPI_TX SIGNAL_TOUCH_SDI
#define SIGNAL_TOUCH_SPI_RX SIGNAL_TOUCH_SDO
#define SIGNAL_TOUCH_SPI_CSN "TOUCH_SPI_CSN"
#define SIGNAL_TOUCH_IRQ "TOUCH_IRQ"
#define SIGNAL_PRESSURE_SENSOR_ADC "PRESSURE_SENSOR_ADC"



// Pirani gauge


// ION Gauge
#define SIGNAL_ION_ENABLE "ION_ENABLE"
#define SIGNAL_ION_DEGAS "ION_DEGAS"


// Turbo Pump

// SWITCH = rocker switch connected to opto coupler.
#define SIGNAL_TURBO_IN_SWITCH_ENABLE "SWITCH_ENABLE_IN"
#define SIGNAL_TURBO_IN_SWITCH_LOW_SPEED "SWITCH_LOW_SPEED_IN"
#define SIGNAL_TURBO_IN_START "START_IN"
#define SIGNAL_TURBO_IN_FAULT "FAULT_IN"
#define SIGNAL_TURBO_IN_LOW_SPEED "LOW_SPEED_IN"
#define SIGNAL_TURBO_OUT_ENABLE "ENABLE_OUT"
#define SIGNAL_TURBO_OUT_LOW_SPEED "LOW_SPEED_OUT"


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
