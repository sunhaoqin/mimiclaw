#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"
#include "tools/tool_delay.h"
#include "tools/tool_sequence.h"
#include "tools/tool_ws2812.h"
#include "tools/tool_stepper.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 24

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register gpio_config */
    mimi_tool_t gc = {
        .name = "gpio_config",
        .description = "Configure GPIO pin mode with optional pull-up/pull-down resistors. Must be called before using a pin.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number (0-48)\",\"minimum\":0,\"maximum\":48},"
            "\"mode\":{\"type\":\"string\",\"description\":\"Pin mode\",\"enum\":[\"input\",\"output\",\"input_output\"]},"
            "\"pull_up\":{\"type\":\"boolean\",\"description\":\"Enable internal pull-up resistor (default: false)\"},"
            "\"pull_down\":{\"type\":\"boolean\",\"description\":\"Enable internal pull-down resistor (default: false)\"}"
            "},"
            "\"required\":[\"pin\",\"mode\"]}",
        .execute = tool_gpio_config_execute,
    };
    register_tool(&gc);

    /* Register gpio_set */
    mimi_tool_t gs = {
        .name = "gpio_set",
        .description = "Set GPIO output level (high/low). Pin must be configured as output first using gpio_config.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number (0-48)\",\"minimum\":0,\"maximum\":48},"
            "\"value\":{\"type\":\"integer\",\"description\":\"Output level\",\"enum\":[0,1]}"
            "},"
            "\"required\":[\"pin\",\"value\"]}",
        .execute = tool_gpio_set_execute,
    };
    register_tool(&gs);

    /* Register gpio_read */
    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read GPIO input level. Returns 0 (low) or 1 (high). Pin should be configured as input first.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number (0-48)\",\"minimum\":0,\"maximum\":48}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    /* Register gpio_pwm */
    mimi_tool_t gp = {
        .name = "gpio_pwm",
        .description = "Configure PWM output on GPIO pin using LEDC peripheral. Can enable/disable PWM or update frequency/duty.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number (0-48)\",\"minimum\":0,\"maximum\":48},"
            "\"frequency\":{\"type\":\"integer\",\"description\":\"PWM frequency in Hz (1-40000000)\",\"minimum\":1,\"maximum\":40000000},"
            "\"duty\":{\"type\":\"number\",\"description\":\"Duty cycle percentage 0.0-100.0, supports decimals for precise servo control (e.g., 7.5 for 1.5ms@50Hz)\",\"minimum\":0.0,\"maximum\":100.0},"
            "\"resolution_bits\":{\"type\":\"integer\",\"description\":\"PWM resolution in bits (1-14, default: 10). Use 10+ for low frequencies like 50Hz servo control\",\"minimum\":1,\"maximum\":14},"
            "\"enable\":{\"type\":\"boolean\",\"description\":\"Enable or disable PWM output (default: true)\"}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_pwm_execute,
    };
    register_tool(&gp);

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

    /* Register ws2812_init */
    mimi_tool_t w2i = {
        .name = "ws2812_init",
        .description = "Initialize WS2812 LED strip on specified GPIO pin. Must be called before other ws2812 commands.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number (0-48, excluding restricted pins)\",\"minimum\":0,\"maximum\":48},"
            "\"num_leds\":{\"type\":\"integer\",\"description\":\"Number of LEDs in the strip (1-256)\",\"minimum\":1,\"maximum\":256}"
            "},"
            "\"required\":[\"pin\",\"num_leds\"]}",
        .execute = tool_ws2812_init_execute,
    };
    register_tool(&w2i);

    /* Register ws2812_set */
    mimi_tool_t w2s = {
        .name = "ws2812_set",
        .description = "Set color of a single LED or all LEDs on the strip. Colors are specified as {r,g,b} values 0-255.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number where strip is initialized\",\"minimum\":0,\"maximum\":48},"
            "\"index\":{\"type\":\"integer\",\"description\":\"LED index (0-based), omit to set all LEDs\"},"
            "\"color\":{\"type\":\"object\",\"description\":\"RGB color values\",\"properties\":{"
            "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
            "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
            "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
            "},\"required\":[\"r\",\"g\",\"b\"]}"
            "},"
            "\"required\":[\"pin\",\"color\"]}",
        .execute = tool_ws2812_set_execute,
    };
    register_tool(&w2s);

    /* Register ws2812_clear */
    mimi_tool_t w2c = {
        .name = "ws2812_clear",
        .description = "Turn off all LEDs on the strip (set to black).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number where strip is initialized\",\"minimum\":0,\"maximum\":48}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_ws2812_clear_execute,
    };
    register_tool(&w2c);

    /* Register stepper_init */
    mimi_tool_t sti = {
        .name = "stepper_init",
        .description = "Initialize DRV8825 stepper motor driver. Configure GPIO pins for STEP, DIR, and ENABLE. Microstep mode is set by external DIP switch.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"step_pin\":{\"type\":\"integer\",\"description\":\"GPIO pin for STEP signal (also used as motor ID)\",\"minimum\":0,\"maximum\":48},"
            "\"dir_pin\":{\"type\":\"integer\",\"description\":\"GPIO pin for DIR signal\",\"minimum\":0,\"maximum\":48},"
            "\"enable_pin\":{\"type\":\"integer\",\"description\":\"GPIO pin for ENABLE signal (active low)\",\"minimum\":0,\"maximum\":48},"
            "\"microstep\":{\"type\":\"integer\",\"description\":\"Microstep mode configured by DIP switch: 1/2/4/8/16/32, default 16\",\"enum\":[1,2,4,8,16,32]}"
            "},"
            "\"required\":[\"step_pin\",\"dir_pin\",\"enable_pin\"]}",
        .execute = tool_stepper_init_execute,
    };
    register_tool(&sti);

    /* Register stepper_move */
    mimi_tool_t stm = {
        .name = "stepper_move",
        .description = "Move stepper motor with trapezoidal acceleration/deceleration. Runs in background task.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"step_pin\":{\"type\":\"integer\",\"description\":\"STEP pin to identify motor instance\",\"minimum\":0,\"maximum\":48},"
            "\"steps\":{\"type\":\"integer\",\"description\":\"Number of steps to move (1-100000)\",\"minimum\":1,\"maximum\":100000},"
            "\"direction\":{\"type\":\"string\",\"description\":\"Rotation direction\",\"enum\":[\"cw\",\"ccw\"]},"
            "\"speed_max\":{\"type\":\"integer\",\"description\":\"Maximum speed in Hz (100-5000), default 2000\",\"minimum\":100,\"maximum\":5000},"
            "\"speed_start\":{\"type\":\"integer\",\"description\":\"Start speed in Hz (100-speed_max), default 200\",\"minimum\":100,\"maximum\":5000},"
            "\"accel_ratio\":{\"type\":\"number\",\"description\":\"Acceleration/deceleration ratio 0.0-0.5, default 0.2\",\"minimum\":0.0,\"maximum\":0.5}"
            "},"
            "\"required\":[\"step_pin\",\"steps\",\"direction\"]}",
        .execute = tool_stepper_move_execute,
    };
    register_tool(&stm);

    /* Register stepper_stop */
    mimi_tool_t sts = {
        .name = "stepper_stop",
        .description = "Stop stepper motor immediately.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"step_pin\":{\"type\":\"integer\",\"description\":\"STEP pin to identify motor instance\",\"minimum\":0,\"maximum\":48}"
            "},"
            "\"required\":[\"step_pin\"]}",
        .execute = tool_stepper_stop_execute,
    };
    register_tool(&sts);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
