# Delay Execute and Tool Sequence Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement two new tools: `delay_execute` for millisecond-precision delayed tool execution and `tool_sequence` for sequential execution of multiple tool steps.

**Architecture:** Both tools will follow the existing tool pattern in main/tools/. `delay_execute` uses FreeRTOS vTaskDelay for blocking delay, then invokes target tool via tool_registry_execute(). `tool_sequence` iterates through steps array, executing each tool sequentially with detailed JSONL logging to /spiffs/logs/.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, cJSON, SPIFFS

---

### Task 1: Create tool_delay.h header file

**Files:**
- Create: `main/tools/tool_delay.h`

**Step 1: Write header file**

```c
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
```

**Step 2: Commit**

```bash
git add main/tools/tool_delay.h
git commit -m "feat: add delay_execute tool header"
```

---

### Task 2: Create tool_delay.c implementation

**Files:**
- Create: `main/tools/tool_delay.c`

**Step 1: Write implementation file**

```c
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
    const char *tool_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tool_name"));
    if (!tool_name || strlen(tool_name) == 0) {
        snprintf(output, output_size, "Error: missing or invalid 'tool_name' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Prevent recursive delay_execute calls */
    if (strcmp(tool_name, "delay_execute") == 0) {
        snprintf(output, output_size, "Error: delay_execute cannot call itself");
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
```

**Step 2: Commit**

```bash
git add main/tools/tool_delay.c
git commit -m "feat: implement delay_execute tool with blocking delay"
```

---

### Task 3: Create tool_sequence.h header file

**Files:**
- Create: `main/tools/tool_sequence.h`

**Step 1: Write header file**

```c
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
```

**Step 2: Commit**

```bash
git add main/tools/tool_sequence.h
git commit -m "feat: add tool_sequence header"
```

---

### Task 4: Create tool_sequence.c implementation

**Files:**
- Create: `main/tools/tool_sequence.c`

**Step 1: Write implementation file**

```c
#include "tools/tool_sequence.h"
#include "tools/tool_registry.h"
#include "mimi_config.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_vfs.h"
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
```

**Step 2: Commit**

```bash
git add main/tools/tool_sequence.c
git commit -m "feat: implement tool_sequence with logging and fast-fail"
```

---

### Task 5: Update tool_registry.c to register new tools

**Files:**
- Modify: `main/tools/tool_registry.c`

**Step 1: Add includes**

After line 7, add:
```c
#include "tools/tool_delay.h"
#include "tools/tool_sequence.h"
```

**Step 2: Add tool registrations before build_tools_json() call**

After the gpio_pwm registration (around line 242), add:

```c
    /* Register delay_execute */
    mimi_tool_t de = {
        .name = "delay_execute",
        .description = "Execute a tool after a specified delay. Blocks until delay completes and target tool executes.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"delay_ms\":{\"type\":\"integer\",\"description\":\"Delay in milliseconds (0-3600000)\",\"minimum\":0,\"maximum\":3600000},"
            "\"tool_name\":{\"type\":\"string\",\"description\":\"Name of tool to execute after delay\"},"
            "\"tool_args\":{\"type\":\"object\",\"description\":\"Arguments for the target tool (default: {})\"}"
            "},"
            "\"required\":[\"delay_ms\",\"tool_name\"]}",
        .execute = tool_delay_execute,
    };
    register_tool(&de);

    /* Register tool_sequence */
    mimi_tool_t ts = {
        .name = "tool_sequence",
        .description = "Execute a sequence of tools sequentially. Stops on first failure (fast-fail). Max 20 steps. Logs to /spiffs/logs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"steps\":{\"type\":\"array\",\"description\":\"Array of tool execution steps (max 20)\","
            "\"items\":{\"type\":\"object\",\"properties\":{"
            "\"tool\":{\"type\":\"string\",\"description\":\"Tool name to execute\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Tool arguments\"}"
            "},\"required\":[\"tool\"]}}"
            "},"
            "\"required\":[\"steps\"]}",
        .execute = tool_sequence_execute,
    };
    register_tool(&ts);
```

**Step 3: Commit**

```bash
git add main/tools/tool_registry.c
git commit -m "feat: register delay_execute and tool_sequence tools"
```

---

### Task 6: Update CMakeLists.txt to include new source files

**Files:**
- Modify: `main/CMakeLists.txt`

**Step 1: Add new source files to SRCS list**

After `"tools/tool_gpio.c"` (around line 22), add:
```
        "tools/tool_delay.c"
        "tools/tool_sequence.c"
```

**Step 2: Commit**

```bash
git add main/CMakeLists.txt
git commit -m "build: add tool_delay.c and tool_sequence.c to CMakeLists"
```

---

### Task 7: Build and verify

**Files:**
- None

**Step 1: Build the project**

```bash
cd /workspaces/mimiclaw
idf.py build
```

Expected: Build succeeds with no errors.

**Step 2: Verify tool count**

Check that tools are registered by looking for log output:
```
I tools: Tools JSON built (X tools)
```
Should show increased count (was 16, now 18).

**Step 3: Commit any fixes if needed**

If build errors, fix and commit with:
```bash
git add <files>
git commit -m "fix: resolve build errors in delay/sequence tools"
```

---

## Summary

This implementation adds:

1. **delay_execute** - Millisecond-precision blocking delay before tool execution
   - Validates delay range (0-3600000ms)
   - Prevents recursive calls
   - Returns target tool's result after execution

2. **tool_sequence** - Sequential tool execution with:
   - Max 20 steps limit
   - Fast-fail on first error
   - JSONL logging to /spiffs/logs/tool_sequence_YYYYMMDD_HHMMSS.jsonl
   - Detailed execution results in response

Both tools follow the existing patterns and integrate cleanly with the tool registry.
