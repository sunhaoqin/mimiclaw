#include "tools/tool_sequence.h"
#include "tools/tool_registry.h"
#include "mimi_config.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <sys/stat.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "tool_sequence";

#define MAX_SEQUENCE_STEPS  20
#define LOG_BUFFER_SIZE     1024

static void ensure_log_dir_exists(void)
{
    struct stat st;
    const char *log_dir = MIMI_SPIFFS_BASE "/logs";
    if (stat(log_dir, &st) != 0) {
        mkdir(log_dir, 0755);
    }
}

static void write_log_entry(const char *log_file, int step, const char *tool_name,
                            const char *args, const char *result, bool success)
{
    FILE *fp = fopen(log_file, "a");
    if (!fp) return;

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(entry, "step", step);
    cJSON_AddStringToObject(entry, "tool", tool_name);
    cJSON_AddStringToObject(entry, "args", args ? args : "{}");
    cJSON_AddStringToObject(entry, "result", result ? result : "");
    cJSON_AddBoolToObject(entry, "success", success);

    char *json_str = cJSON_PrintUnformatted(entry);
    if (json_str) {
        fprintf(fp, "%s\n", json_str);
        free(json_str);
    }

    cJSON_Delete(entry);
    fclose(fp);
}

esp_err_t tool_sequence_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse steps array */
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!steps || !cJSON_IsArray(steps)) {
        snprintf(output, output_size, "Error: missing or invalid 'steps' array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int step_count = cJSON_GetArraySize(steps);
    if (step_count == 0) {
        snprintf(output, output_size, "Error: 'steps' array is empty");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (step_count > MAX_SEQUENCE_STEPS) {
        snprintf(output, output_size, "Error: too many steps (max %d)", MAX_SEQUENCE_STEPS);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Create log file */
    ensure_log_dir_exists();
    char log_file[128];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(log_file, sizeof(log_file),
             MIMI_SPIFFS_BASE "/logs/tool_sequence_%04d%02d%02d_%02d%02d%02d.jsonl",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    ESP_LOGI(TAG, "Starting sequence with %d steps, logging to %s", step_count, log_file);

    /* Execute steps */
    cJSON *results_array = cJSON_CreateArray();
    int64_t start_time = esp_timer_get_time();

    for (int i = 0; i < step_count; i++) {
        cJSON *step = cJSON_GetArrayItem(steps, i);
        if (!step || !cJSON_IsObject(step)) {
            cJSON_Delete(root);
            cJSON_Delete(results_array);
            snprintf(output, output_size, "Error: step %d is not a valid object", i);
            return ESP_ERR_INVALID_ARG;
        }

        /* Parse tool name */
        const char *tool_name = cJSON_GetStringValue(cJSON_GetObjectItem(step, "tool"));
        if (!tool_name || strlen(tool_name) == 0) {
            cJSON_Delete(root);
            cJSON_Delete(results_array);
            snprintf(output, output_size, "Error: step %d missing 'tool' field", i);
            return ESP_ERR_INVALID_ARG;
        }

        /* Parse args (optional) */
        cJSON *args_obj = cJSON_GetObjectItem(step, "args");
        char *args_str = NULL;
        if (args_obj && cJSON_IsObject(args_obj)) {
            args_str = cJSON_PrintUnformatted(args_obj);
        } else {
            args_str = strdup("{}");
        }

        ESP_LOGI(TAG, "Executing step %d: %s", i, tool_name);

        /* Execute tool */
        char tool_output[512] = {0};
        esp_err_t err = tool_registry_execute(tool_name, args_str ? args_str : "{}",
                                              tool_output, sizeof(tool_output));

        /* Record result */
        cJSON *result_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(result_obj, "step", i);
        cJSON_AddStringToObject(result_obj, "tool", tool_name);
        cJSON_AddBoolToObject(result_obj, "success", err == ESP_OK);
        cJSON_AddStringToObject(result_obj, "result", tool_output);
        cJSON_AddItemToArray(results_array, result_obj);

        /* Write to log */
        write_log_entry(log_file, i, tool_name, args_str, tool_output, err == ESP_OK);

        free(args_str);

        /* Fast-fail on error */
        if (err != ESP_OK) {
            cJSON_Delete(root);

            int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;

            /* Build error response */
            cJSON *error_response = cJSON_CreateObject();
            cJSON_AddStringToObject(error_response, "status", "failed");
            cJSON_AddNumberToObject(error_response, "failed_at", i);
            cJSON_AddStringToObject(error_response, "error", tool_output);
            cJSON_AddNumberToObject(error_response, "duration_ms", (double)elapsed_ms);
            cJSON_AddStringToObject(error_response, "log_file", log_file);
            cJSON_AddItemToObject(error_response, "steps_executed", results_array);

            char *error_json = cJSON_PrintUnformatted(error_response);
            snprintf(output, output_size, "%s", error_json ? error_json : "Error: sequence failed");
            free(error_json);
            cJSON_Delete(error_response);

            ESP_LOGW(TAG, "Sequence failed at step %d: %s", i, tool_output);
            return err;
        }
    }

    cJSON_Delete(root);

    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;

    /* Build success response */
    cJSON *success_response = cJSON_CreateObject();
    cJSON_AddStringToObject(success_response, "status", "success");
    cJSON_AddNumberToObject(success_response, "steps_executed", step_count);
    cJSON_AddNumberToObject(success_response, "duration_ms", (double)elapsed_ms);
    cJSON_AddStringToObject(success_response, "log_file", log_file);
    cJSON_AddItemToObject(success_response, "steps", results_array);

    char *success_json = cJSON_PrintUnformatted(success_response);
    snprintf(output, output_size, "%s", success_json ? success_json : "OK: sequence completed");
    free(success_json);
    cJSON_Delete(success_response);

    ESP_LOGI(TAG, "Sequence completed: %d steps in %lld ms", step_count, (long long)elapsed_ms);
    return ESP_OK;
}
