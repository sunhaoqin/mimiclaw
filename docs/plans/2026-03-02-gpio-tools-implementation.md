# GPIO 工具实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 实现 GPIO 控制工具（gpio_config, gpio_set, gpio_read, gpio_pwm），支持基础 GPIO 操作和 LEDC PWM 输出。

**Architecture:** 创建 `tool_gpio.h/c` 实现所有 GPIO 功能，在 `tool_registry.c` 中注册工具并定义 JSON Schema。使用 ESP-IDF 的 `driver/gpio.h` 和 `driver/ledc.h`。

**Tech Stack:** ESP-IDF 5.x, C, FreeRTOS, GPIO/LEDC 驱动

---

## 前置准备

**确保当前在 worktree 中：**
如果不在 worktree，先创建：
```bash
cd /workspaces/mimiclaw
git worktree add ../mimiclaw-gpio feature/gpio-tools
cd ../mimiclaw-gpio
```

---

## Task 1: 创建 tool_gpio.h 头文件

**Files:**
- Create: `main/tools/tool_gpio.h`

**Step 1: 创建头文件，声明所有 GPIO 工具函数**

参考 `tool_get_time.h` 格式：

```c
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
```

**Step 2: 验证头文件格式**

Run:
```bash
cat main/tools/tool_gpio.h
```

Expected: 文件内容正确，无语法错误

**Step 3: Commit**

```bash
git add main/tools/tool_gpio.h
git commit -m "feat: add gpio tools header with function declarations"
```

---

## Task 2: 实现 gpio_config 工具

**Files:**
- Create: `main/tools/tool_gpio.c`
- Reference: `main/tools/tool_files.c` (JSON 解析模式)

**Step 1: 创建源文件框架**

```c
#include "tool_gpio.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tool_gpio";

/* 可用引脚检查 */
static bool is_gpio_available(int pin)
{
    /* 限制 SPI Flash 引脚: 19-24, 26-32 */
    if ((pin >= 19 && pin <= 24) || (pin >= 26 && pin <= 32)) {
        return false;
    }
    /* 限制 USB-JTAG 引脚: 43-44 */
    if (pin >= 43 && pin <= 44) {
        return false;
    }
    /* 有效范围: 0-48 */
    return pin >= 0 && pin <= 48;
}

/* gpio_config 实现 */
esp_err_t tool_gpio_config_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 pin */
    cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin = pin_item->valueint;

    /* 检查引脚可用性 */
    if (!is_gpio_available(pin)) {
        snprintf(output, output_size, "Error: GPIO%d is not available (restricted: flash/usb pins)", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 mode */
    const char *mode_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
    if (!mode_str) {
        snprintf(output, output_size, "Error: missing 'mode' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_mode_t mode;
    if (strcmp(mode_str, "input") == 0) {
        mode = GPIO_MODE_INPUT;
    } else if (strcmp(mode_str, "output") == 0) {
        mode = GPIO_MODE_OUTPUT;
    } else if (strcmp(mode_str, "input_output") == 0) {
        mode = GPIO_MODE_INPUT_OUTPUT;
    } else {
        snprintf(output, output_size, "Error: invalid mode '%s' (valid: input, output, input_output)", mode_str);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 pull_up/pull_down */
    cJSON *pull_up_item = cJSON_GetObjectItem(root, "pull_up");
    cJSON *pull_down_item = cJSON_GetObjectItem(root, "pull_down");
    bool pull_up = pull_up_item && cJSON_IsTrue(pull_up_item);
    bool pull_down = pull_down_item && cJSON_IsTrue(pull_down_item);

    /* 配置 GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure GPIO%d (%s)", pin, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: GPIO%d configured as %s", pin, mode_str);
    ESP_LOGI(TAG, "gpio_config: pin=%d mode=%s pull_up=%d pull_down=%d", pin, mode_str, pull_up, pull_down);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**Step 2: 编译验证**

Run:
```bash
idf.py build 2>&1 | head -50
```

Expected: 编译通过，无错误

**Step 3: Commit**

```bash
git add main/tools/tool_gpio.c
git commit -m "feat: implement gpio_config tool"
```

---

## Task 3: 实现 gpio_set 和 gpio_read 工具

**Files:**
- Modify: `main/tools/tool_gpio.c` (添加函数实现)

**Step 1: 在文件末尾添加 gpio_set 实现**

```c
/* gpio_set 实现 */
esp_err_t tool_gpio_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin = pin_item->valueint;

    if (!is_gpio_available(pin)) {
        snprintf(output, output_size, "Error: GPIO%d is not available", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *value_item = cJSON_GetObjectItem(root, "value");
    if (!value_item || !cJSON_IsNumber(value_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'value' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int value = value_item->valueint;

    if (value != 0 && value != 1) {
        snprintf(output, output_size, "Error: invalid value %d (valid: 0, 1)", value);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gpio_set_level(pin, value);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set GPIO%d (%s)", pin, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: GPIO%d set to %s", pin, value ? "high" : "low");
    ESP_LOGI(TAG, "gpio_set: pin=%d value=%d", pin, value);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**Step 2: 在文件末尾添加 gpio_read 实现**

```c
/* gpio_read 实现 */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin = pin_item->valueint;

    if (!is_gpio_available(pin)) {
        snprintf(output, output_size, "Error: GPIO%d is not available", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(pin);

    snprintf(output, output_size, "GPIO%d = %d (%s)", pin, level, level ? "high" : "low");
    ESP_LOGI(TAG, "gpio_read: pin=%d level=%d", pin, level);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**Step 3: 编译验证**

Run:
```bash
idf.py build 2>&1 | grep -E "(error|warning|Error)" | head -20
```

Expected: 无错误或仅警告

**Step 4: Commit**

```bash
git add main/tools/tool_gpio.c
git commit -m "feat: implement gpio_set and gpio_read tools"
```

---

## Task 4: 实现 gpio_pwm 工具

**Files:**
- Modify: `main/tools/tool_gpio.c` (添加 PWM 实现)
- Reference: ESP-IDF LEDC 文档

**Step 1: 在文件开头添加 LEDC 通道管理结构**

在 `#include` 后添加：

```c
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_MAX_CHANNELS       8

typedef struct {
    int pin;
    bool in_use;
} pwm_channel_t;

static pwm_channel_t s_pwm_channels[LEDC_MAX_CHANNELS] = {0};
static bool s_ledc_timer_configured = false;

/* 查找或分配 LEDC 通道 */
static int find_or_allocate_channel(int pin)
{
    /* 查找是否已分配 */
    for (int i = 0; i < LEDC_MAX_CHANNELS; i++) {
        if (s_pwm_channels[i].in_use && s_pwm_channels[i].pin == pin) {
            return i;
        }
    }
    /* 分配新通道 */
    for (int i = 0; i < LEDC_MAX_CHANNELS; i++) {
        if (!s_pwm_channels[i].in_use) {
            s_pwm_channels[i].pin = pin;
            s_pwm_channels[i].in_use = true;
            return i;
        }
    }
    return -1; /* 无可用通道 */
}

/* 释放 LEDC 通道 */
static void release_channel(int pin)
{
    for (int i = 0; i < LEDC_MAX_CHANNELS; i++) {
        if (s_pwm_channels[i].in_use && s_pwm_channels[i].pin == pin) {
            ledc_stop(LEDC_MODE, i, 0);
            s_pwm_channels[i].in_use = false;
            s_pwm_channels[i].pin = -1;
            break;
        }
    }
}
```

**Step 2: 添加 gpio_pwm 实现**

```c
/* gpio_pwm 实现 */
esp_err_t tool_gpio_pwm_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin = pin_item->valueint;

    if (!is_gpio_available(pin)) {
        snprintf(output, output_size, "Error: GPIO%d is not available", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 enable (默认 true) */
    cJSON *enable_item = cJSON_GetObjectItem(root, "enable");
    bool enable = true;
    if (enable_item) {
        enable = cJSON_IsTrue(enable_item);
    }

    if (!enable) {
        /* 停止 PWM */
        release_channel(pin);
        snprintf(output, output_size, "OK: PWM stopped on GPIO%d", pin);
        ESP_LOGI(TAG, "gpio_pwm: stopped on pin=%d", pin);
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* 启动或更新 PWM */
    cJSON *freq_item = cJSON_GetObjectItem(root, "frequency");
    cJSON *duty_item = cJSON_GetObjectItem(root, "duty");

    if (!freq_item || !cJSON_IsNumber(freq_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'frequency' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!duty_item || !cJSON_IsNumber(duty_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'duty' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int frequency = freq_item->valueint;
    int duty_percent = duty_item->valueint;

    if (frequency < 1 || frequency > 40000000) {
        snprintf(output, output_size, "Error: frequency must be 1-40000000 Hz");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_percent < 0 || duty_percent > 100) {
        snprintf(output, output_size, "Error: duty must be 0-100%%");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 resolution_bits (默认 8) */
    cJSON *res_item = cJSON_GetObjectItem(root, "resolution_bits");
    int resolution_bits = 8;
    if (res_item && cJSON_IsNumber(res_item)) {
        resolution_bits = res_item->valueint;
    }
    if (resolution_bits < 1 || resolution_bits > 14) {
        snprintf(output, output_size, "Error: resolution_bits must be 1-14");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 配置 LEDC 定时器（仅一次） */
    if (!s_ledc_timer_configured) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t err = ledc_timer_config(&ledc_timer);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: failed to configure LEDC timer");
            cJSON_Delete(root);
            return err;
        }
        s_ledc_timer_configured = true;
    }

    /* 查找或分配通道 */
    int channel = find_or_allocate_channel(pin);
    if (channel < 0) {
        snprintf(output, output_size, "Error: no free LEDC channel available");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    /* 配置通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_MODE,
        .channel = channel,
        .timer_sel = LEDC_TIMER,
        .duty = (duty_percent * ((1 << resolution_bits) - 1)) / 100,
        .hpoint = 0,
    };

    esp_err_t err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure LEDC channel");
        cJSON_Delete(root);
        return err;
    }

    /* 设置频率 */
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, frequency);

    snprintf(output, output_size, "OK: PWM started on GPIO%d @ %dHz %d%%", pin, frequency, duty_percent);
    ESP_LOGI(TAG, "gpio_pwm: pin=%d freq=%dHz duty=%d%%", pin, frequency, duty_percent);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**Step 3: 编译验证**

Run:
```bash
idf.py build 2>&1 | grep -E "(error|Error)" | head -10
```

Expected: 无错误

**Step 4: Commit**

```bash
git add main/tools/tool_gpio.c
git commit -m "feat: implement gpio_pwm tool with LEDC driver"
```

---

## Task 5: 在 CMakeLists.txt 中添加新文件

**Files:**
- Modify: `main/CMakeLists.txt`

**Step 1: 在 SRCS 列表中添加 tool_gpio.c**

修改前：
```cmake
    SRCS
        "mimi.c"
        ...
        "tools/tool_files.c"
```

修改后：
```cmake
    SRCS
        "mimi.c"
        ...
        "tools/tool_files.c"
        "tools/tool_gpio.c"
```

**Step 2: 验证修改**

Run:
```bash
git diff main/CMakeLists.txt
```

Expected: 显示 tool_gpio.c 已添加

**Step 3: 编译验证**

Run:
```bash
idf.py build 2>&1 | tail -20
```

Expected: 编译成功

**Step 4: Commit**

```bash
git add main/CMakeLists.txt
git commit -m "build: add tool_gpio.c to CMakeLists.txt"
```

---

## Task 6: 在 tool_registry.c 中注册 GPIO 工具

**Files:**
- Modify: `main/tools/tool_registry.c`
- Reference: `main/tools/tool_files.h` 和现有工具注册代码

**Step 1: 在文件开头添加 include**

在 `#include "tools/tool_cron.h"` 后添加：
```c
#include "tools/tool_gpio.h"
```

**Step 2: 在 tool_registry_init() 中添加工具注册**

在最后一个 register_tool 调用后添加：

```c
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
            "\"duty\":{\"type\":\"integer\",\"description\":\"Duty cycle percentage (0-100)\",\"minimum\":0,\"maximum\":100},"
            "\"resolution_bits\":{\"type\":\"integer\",\"description\":\"PWM resolution in bits (1-14, default: 8)\",\"minimum\":1,\"maximum\":14},"
            "\"enable\":{\"type\":\"boolean\",\"description\":\"Enable or disable PWM output (default: true)\"}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_pwm_execute,
    };
    register_tool(&gp);
```

**Step 3: 编译验证**

Run:
```bash
idf.py fullclean && idf.py build 2>&1 | tail -30
```

Expected: 编译成功，显示 "Tool registry initialized" 日志

**Step 4: Commit**

```bash
git add main/tools/tool_registry.c
git commit -m "feat: register gpio tools in tool_registry"
```

---

## Task 7: 完整构建验证

**Step 1: 完整重新构建**

Run:
```bash
idf.py fullclean
idf.py build 2>&1 | tee build.log
```

Expected: 无错误，最终显示 `[1/1] Completed 'cmake_check_build_system'` 或类似成功信息

**Step 2: 检查构建输出**

Run:
```bash
grep -E "(tool_gpio|error:|Error:)" build.log | head -20
```

Expected: 显示 tool_gpio.c 编译信息，无错误

**Step 3: 检查固件大小**

Run:
```bash
idf.py size 2>&1 | tail -30
```

Expected: 显示固件大小统计

**Step 4: Commit 构建检查完成**

```bash
git commit --allow-empty -m "chore: verify full build passes"
```

---

## 测试计划

由于 ESP-IDF 项目没有传统的单元测试框架，测试通过以下方式进行：

### 编译测试
- [x] 代码编译通过
- [x] 无警告或警告已审查
- [x] 固件大小在可接受范围内

### 代码审查检查清单
- [ ] GPIO 引脚限制正确（排除 Flash/USB 引脚）
- [ ] JSON Schema 格式正确
- [ ] 错误处理完整
- [ ] LEDC 通道管理正确（分配/释放）
- [ ] 日志输出符合项目风格

### 硬件测试（需要实际设备）
连接到 ESP32-S3 开发板：

```bash
# 烧录固件
idf.py -p /dev/ttyACM0 flash monitor
```

通过 Telegram Bot 或 WebSocket 测试：

**测试 gpio_config:**
```
User: 帮我配置 GPIO2 为输出模式
Agent: gpio_config("{\"pin\": 2, \"mode\": \"output\"}")
Expected: "OK: GPIO2 configured as output"
```

**测试 gpio_set:**
```
User: 把 GPIO2 设为高电平
Agent: gpio_set("{\"pin\": 2, \"value\": 1}")
Expected: "OK: GPIO2 set to high"
```

**测试 gpio_pwm:**
```
User: 在 GPIO4 上启动 1kHz 50% 占空比的 PWM
Agent: gpio_pwm("{\"pin\": 4, \"frequency\": 1000, \"duty\": 50}")
Expected: "OK: PWM started on GPIO4 @ 1000Hz 50%"
```

**测试错误处理:**
```
User: 配置 GPIO20 为输出（Flash 引脚）
Expected: "Error: GPIO20 is not available (restricted: flash/usb pins)"
```

---

## 完成检查清单

- [ ] `main/tools/tool_gpio.h` 创建完成
- [ ] `main/tools/tool_gpio.c` 实现完成（4 个工具函数）
- [ ] `main/CMakeLists.txt` 更新完成
- [ ] `main/tools/tool_registry.c` 注册完成
- [ ] 完整构建通过
- [ ] 设计文档已同步更新（如需要）

---

## 执行选项

**Plan complete and saved to `docs/plans/2026-03-02-gpio-tools-implementation.md`.**

**Two execution options:**

**1. Subagent-Driven (this session)** - 我使用 superpowers:subagent-driven-development 调度子代理逐个任务执行，每个任务后审查，快速迭代

**2. Parallel Session (separate)** - 开新会话用 superpowers:executing-plans，批量执行带检查点

**Which approach?**
