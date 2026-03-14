#include "pressure_sensor.h"

#include <math.h>
#include <stdint.h>

#include "hardware/adc.h"
#include "pico/stdlib.h"

#define PRESSURE_NUM_SAMPLES 100U              // Average N ADC reads to reduce display jitter.
#define PRESSURE_SAMPLE_DELAY_US 100U          // Spacing between ADC samples during one average window.
#define PRESSURE_ADC_MAX_COUNTS 4095.0f        // RP2040 ADC full scale (12-bit: 0..4095).
#define PRESSURE_ADC_ZERO_OFFSET_COUNTS 4.0f   // Measured unplugged baseline (~3 mV at ADC pin).
#define PRESSURE_CONVERSION_FACTOR (10.31f / PRESSURE_ADC_MAX_COUNTS) // Divider slope estimate from ADC counts to sensor-side volts.
// Two-point linear calibration in voltage mode:
// measured 5.000V should be 5.000V, and measured 10.200V should be 10.000V.
#define PRESSURE_VOLTAGE_CAL_GAIN 0.983244f
#define PRESSURE_VOLTAGE_CAL_OFFSET (-0.031264f)
static uint8_t s_pressure_adc_input = 2U;

void pressure_sensor_init(uint8_t adc_gpio) {
    adc_init();
    adc_gpio_init(adc_gpio);
    s_pressure_adc_input = (uint8_t)(adc_gpio - 26U);
    adc_select_input(s_pressure_adc_input);
}

pressure_sensor_reading_t pressure_sensor_read_torr(void) {
    uint32_t sum = 0U;

    adc_select_input(s_pressure_adc_input);

    for (uint32_t i = 0; i < PRESSURE_NUM_SAMPLES; ++i) {
        sum += adc_read();
        sleep_us(PRESSURE_SAMPLE_DELAY_US);
    }

    const float average_counts = (float)sum / (float)PRESSURE_NUM_SAMPLES;
    float corrected_counts = average_counts - PRESSURE_ADC_ZERO_OFFSET_COUNTS;
    if (corrected_counts < 0.0f) {
        corrected_counts = 0.0f;
    }

    float voltage = corrected_counts * PRESSURE_CONVERSION_FACTOR;
    voltage = voltage * PRESSURE_VOLTAGE_CAL_GAIN + PRESSURE_VOLTAGE_CAL_OFFSET;
    if (voltage < 0.0f) {
        voltage = 0.0f;
    }
    pressure_sensor_reading_t reading = {
        .voltage = voltage,
        .pressure_torr = 0.0f,
    };

    // clip min value because it is in the error state
    if (voltage <= PRESSURE_SENSOR_MIN_VALID_VOLTAGE) {
        reading.pressure_torr = powf(10.0f, (PRESSURE_SENSOR_MIN_VALID_VOLTAGE - 6.304f) / 1.286f);
        return reading;
    }

    reading.pressure_torr = powf(10.0f, (voltage - 6.304f) / 1.286f);

    return reading;
}
