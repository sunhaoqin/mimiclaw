#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize DRV8825 stepper motor driver.
 * Configure GPIO pins for STEP, DIR, and ENABLE.
 * Microstep mode is set by external DIP switch, only recorded in software.
 */
esp_err_t tool_stepper_init_execute(const char *input_json, char *output, size_t output_size);

/**
 * Move stepper motor with trapezoidal acceleration/deceleration.
 */
esp_err_t tool_stepper_move_execute(const char *input_json, char *output, size_t output_size);

/**
 * Stop stepper motor immediately.
 */
esp_err_t tool_stepper_stop_execute(const char *input_json, char *output, size_t output_size);
