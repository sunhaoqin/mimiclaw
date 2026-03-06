#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize WS2812 LED strip on specified GPIO pin.
 * Creates RMT channel and configures LED strip.
 */
esp_err_t tool_ws2812_init_execute(const char *input_json, char *output, size_t output_size);

/**
 * Set color of a single LED or all LEDs.
 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Clear all LEDs (turn off).
 */
esp_err_t tool_ws2812_clear_execute(const char *input_json, char *output, size_t output_size);

/**
 * Fill all LEDs with the same color.
 */
esp_err_t tool_ws2812_fill_execute(const char *input_json, char *output, size_t output_size);
