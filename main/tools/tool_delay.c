#include "tools/tool_delay.h"
#include "tools/tool_registry.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "tool_delay";

esp_err_t tool_delay_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse delay_ms */
    cJSON *delay_item = cJSON_GetObjectItem(root, "delay_ms");
    if (!delay_item || !cJSON_IsNumber(delay_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'delay_ms' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int delay_ms = delay_item->valueint;
    if (delay_ms < 0 || delay_ms > 3600000) {  /* Max 1 hour */
        snprintf(output, output_size, "Error: delay_ms must be 0-3600000 (1 hour max)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse tool_name */
    const char *tool_name_ptr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tool_name"));
    if (!tool_name_ptr || strlen(tool_name_ptr) == 0) {
        snprintf(output, output_size, "Error: missing or invalid 'tool_name' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy tool_name before deleting root */
    char tool_name[64];
    strncpy(tool_name, tool_name_ptr, sizeof(tool_name) - 1);
    tool_name[sizeof(tool_name) - 1] = '\0';

    /* Prevent recursive delay_execute calls */
    if (strcmp(tool_name, "delay_execute") == 0) {
        snprintf(output, output_size, "Error: delay_execute cannot call itself");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Prevent calling tool_sequence (to avoid complexity) */
    if (strcmp(tool_name, "tool_sequence") == 0) {
        snprintf(output, output_size, "Error: delay_execute cannot call tool_sequence");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse tool_args (optional, default to empty object) */
    cJSON *tool_args = cJSON_GetObjectItem(root, "tool_args");
    char *args_str = NULL;
    if (tool_args && cJSON_IsObject(tool_args)) {
        args_str = cJSON_PrintUnformatted(tool_args);
    } else {
        args_str = strdup("{}");
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Delaying %d ms before executing tool '%s'", delay_ms, tool_name);

    /* Perform the delay (blocking) */
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    /* Execute the target tool */
    char tool_output[512] = {0};
    esp_err_t err = tool_registry_execute(tool_name, args_str ? args_str : "{}",
                                          tool_output, sizeof(tool_output));
    free(args_str);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: delayed tool execution failed: %s", tool_output);
        return err;
    }

    snprintf(output, output_size, "OK: After %d ms, executed '%s': %s", delay_ms, tool_name, tool_output);
    ESP_LOGI(TAG, "delay_execute: %s", output);
    return ESP_OK;
}
