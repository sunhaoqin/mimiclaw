#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Configure GPIO pin mode with optional pull-up/pull-down resistors.
 */
esp_err_t tool_gpio_config_execute(const char *input_json, char *output, size_t output_size);

/**
 * Set GPIO output level (high/low).
 */
esp_err_t tool_gpio_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Read GPIO input level. Returns 0 (low) or 1 (high).
 */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/**
 * Configure PWM output on GPIO pin using LEDC peripheral.
 */
esp_err_t tool_gpio_pwm_execute(const char *input_json, char *output, size_t output_size);
