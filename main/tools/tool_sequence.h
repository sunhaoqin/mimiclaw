#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a sequence of tools sequentially.
 *
 * JSON input: {
 *   "steps": [
 *     {"tool": "gpio_set", "args": {"pin": 12, "value": 1}},
 *     {"tool": "delay_execute", "args": {"delay_ms": 50, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 0}}}
 *   ]
 * }
 *
 * Each step is executed in order. If any step fails, execution stops immediately (fast-fail).
 * Detailed logs are written to /spiffs/logs/tool_sequence_*.jsonl
 *
 * Max 20 steps allowed to prevent resource exhaustion.
 */
esp_err_t tool_sequence_execute(const char *input_json, char *output, size_t output_size);
