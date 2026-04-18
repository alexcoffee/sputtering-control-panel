#include "current_sensor.h"

#include <stdint.h>

#include "hardware/adc.h"
#include "pico/stdlib.h"

#define CURRENT_NUM_SAMPLES 100U              // Average N ADC reads to reduce display jitter.
#define CURRENT_SAMPLE_DELAY_US 100U          // Spacing between ADC samples during one average window.
#define CURRENT_ADC_MAX_COUNTS 4095.0f        // RP2040 ADC full scale (12-bit: 0..4095).
#define CURRENT_ADC_ZERO_OFFSET_COUNTS 4.0f   // Measured unplugged baseline (~3 mV at ADC pin).
#define CURRENT_CONVERSION_FACTOR (10.31f / CURRENT_ADC_MAX_COUNTS) // Divider slope estimate from ADC counts to sensor-side volts.
// Two-point linear calibration in voltage mode:
// measured 5.000V should be 5.000V, and measured 10.200V should be 10.000V.
#define CURRENT_VOLTAGE_CAL_GAIN 0.983244f
#define CURRENT_VOLTAGE_CAL_OFFSET (-0.031264f)
#define CURRENT_VOLTS_PER_AMP 2.0f
static uint8_t s_current_adc_input = 2U;

void current_sensor_init(uint8_t adc_gpio) {
    adc_init();
    adc_gpio_init(adc_gpio);
    s_current_adc_input = (uint8_t)(adc_gpio - 26U);
    adc_select_input(s_current_adc_input);
}

current_sensor_reading_t current_sensor_read_amps(void) {
    uint32_t sum = 0U;

    adc_select_input(s_current_adc_input);

    for (uint32_t i = 0; i < CURRENT_NUM_SAMPLES; ++i) {
        sum += adc_read();
        sleep_us(CURRENT_SAMPLE_DELAY_US);
    }

    const float average_counts = (float)sum / (float)CURRENT_NUM_SAMPLES;
    float corrected_counts = average_counts - CURRENT_ADC_ZERO_OFFSET_COUNTS;
    if (corrected_counts < 0.0f) {
        corrected_counts = 0.0f;
    }

    float voltage = corrected_counts * CURRENT_CONVERSION_FACTOR;
    voltage = voltage * CURRENT_VOLTAGE_CAL_GAIN + CURRENT_VOLTAGE_CAL_OFFSET;
    if (voltage < 0.0f) {
        voltage = 0.0f;
    }
    current_sensor_reading_t reading = {
        .voltage = voltage,
        .current_amps = 0.0f,
    };

    reading.current_amps = voltage / CURRENT_VOLTS_PER_AMP;

    return reading;
}
