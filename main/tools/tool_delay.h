#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a tool after a specified delay.
 *
 * JSON input: {
 *   "delay_ms": 3000,
 *   "tool_name": "gpio_set",
 *   "tool_args": {"pin": 12, "value": 0}
 * }
 *
 * This function blocks for the specified delay, then executes the target tool.
 * The target tool's output is captured and returned.
 */
esp_err_t tool_delay_execute(const char *input_json, char *output, size_t output_size);
