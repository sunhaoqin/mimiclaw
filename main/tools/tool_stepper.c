#include "tool_stepper.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "tool_stepper";

/* 最大支持的电机实例数 */
#define STEPPER_MAX_MOTORS      2

/* 默认速度参数 */
#define DEFAULT_SPEED_MAX       2000    // Hz
#define DEFAULT_SPEED_START     200     // Hz
#define DEFAULT_ACCEL_RATIO     0.2     // 20%

/* RMT 分辨率: 1MHz = 1us/tick */
#define STEPPER_RMT_RESOLUTION_HZ   1000000

/* 电机实例状态 */
typedef struct {
    int pin_step;           // STEP 引脚
    int pin_dir;            // DIR 引脚
    int pin_enable;         // ENABLE 引脚
    int microstep;          // 微步模式（由外部拨码开关配置）
    bool in_use;
    bool is_moving;         // 是否正在转动
    bool stop_request;      // 停止请求标志
    TaskHandle_t task_handle;   // 转动任务句柄
    rmt_channel_handle_t rmt_chan;  // RMT 通道
} stepper_motor_t;

static stepper_motor_t s_motors[STEPPER_MAX_MOTORS] = {0};

/* 运动参数结构 */
typedef struct {
    int steps_total;        // 总步数
    int speed_start;        // 起始速度 (Hz)
    int speed_max;          // 最大速度 (Hz)
    float accel_ratio;      // 加减速比例
} motion_profile_t;

/* 任务参数结构 */
typedef struct {
    stepper_motor_t *motor;
    motion_profile_t profile;
    bool direction;         // true = cw, false = ccw
} move_task_params_t;

/* 查找电机实例 */
static stepper_motor_t* find_motor(int pin_step)
{
    for (int i = 0; i < STEPPER_MAX_MOTORS; i++) {
        if (s_motors[i].in_use && s_motors[i].pin_step == pin_step) {
            return &s_motors[i];
        }
    }
    return NULL;
}

/* 分配电机实例 */
static stepper_motor_t* allocate_motor(void)
{
    for (int i = 0; i < STEPPER_MAX_MOTORS; i++) {
        if (!s_motors[i].in_use) {
            s_motors[i].in_use = true;
            return &s_motors[i];
        }
    }
    return NULL;
}

/* 释放电机实例 */
static void free_motor(stepper_motor_t *motor)
{
    if (motor->rmt_chan) {
        rmt_del_channel(motor->rmt_chan);
        motor->rmt_chan = NULL;
    }
    if (motor->task_handle) {
        vTaskDelete(motor->task_handle);
        motor->task_handle = NULL;
    }
    motor->in_use = false;
    motor->pin_step = -1;
}

/* 检查 GPIO 是否可用 */
static bool is_gpio_available(int pin)
{
    /* 限制 SPI Flash 引脚: 19-24, 26-32 */
    if ((pin >= 19 && pin <= 24) || (pin >= 26 && pin <= 32)) {
        /* 白名单: 21(EN), 25, 32, 33 允许使用 */
        if (pin == 21 || pin == 25 || pin == 32 || pin == 33) {
            return true;
        }
        return false;
    }
    /* 限制 USB-JTAG 引脚: 43-44 */
    if (pin >= 43 && pin <= 44) {
        return false;
    }
    /* 有效范围: 0-48 */
    return pin >= 0 && pin <= 48;
}

/* 步进电机编码器回调 - 生成脉冲序列 */
static size_t stepper_encoder_callback(const void *data, size_t data_size,
                                        size_t symbols_written, size_t symbols_free,
                                        rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    /* data 是 uint32_t 数组: [周期1_us, 周期2_us, ..., 0(结束标记)] */
    const uint32_t *periods = (const uint32_t *)data;
    size_t symbol_pos = 0;

    while (symbol_pos < symbols_free && periods[symbols_written + symbol_pos] != 0) {
        uint32_t period_us = periods[symbols_written + symbol_pos];
        uint32_t high_us = period_us / 2;   // 50% 占空比
        uint32_t low_us = period_us - high_us;

        /* 限制最大时间，避免 RMT 符号溢出 (15位 = 32767 ticks) */
        if (high_us > 30000) high_us = 30000;
        if (low_us > 30000) low_us = 30000;

        symbols[symbol_pos].level0 = 1;
        symbols[symbol_pos].duration0 = high_us;
        symbols[symbol_pos].level1 = 0;
        symbols[symbol_pos].duration1 = low_us;

        symbol_pos++;
    }

    /* 检查是否发送完毕 */
    if (periods[symbols_written + symbol_pos] == 0) {
        *done = true;
    }

    return symbol_pos;
}

/* 电机转动任务（在后台执行） */
static void stepper_move_task(void *pvParameters)
{
    move_task_params_t *params = (move_task_params_t *)pvParameters;
    stepper_motor_t *motor = params->motor;
    motion_profile_t *profile = &params->profile;

    /* 设置方向 */
    gpio_set_level(motor->pin_dir, params->direction ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(5));  // 方向稳定延时

    /* 使能驱动 */
    gpio_set_level(motor->pin_enable, 0);  // 低电平使能
    vTaskDelay(pdMS_TO_TICKS(2));

    /* 计算加减速步数 */
    int accel_steps = (int)(profile->steps_total * profile->accel_ratio);
    int decel_steps = accel_steps;
    int const_steps = profile->steps_total - accel_steps - decel_steps;

    /* 如果步数太少，调整加减速 */
    if (const_steps < 0) {
        accel_steps = profile->steps_total / 2;
        decel_steps = profile->steps_total - accel_steps;
        const_steps = 0;
    }

    ESP_LOGI(TAG, "Move start: steps=%d, accel=%d, const=%d, decel=%d",
             profile->steps_total, accel_steps, const_steps, decel_steps);

    /* 分配周期数组 (每步一个周期，单位为 us，以0结束) */
    uint32_t *periods = calloc(profile->steps_total + 1, sizeof(uint32_t));
    if (!periods) {
        ESP_LOGE(TAG, "Failed to allocate periods array");
        goto cleanup;
    }

    /* 填充周期数组 - 加速阶段 */
    for (int i = 0; i < accel_steps; i++) {
        float ratio = (float)i / accel_steps;
        int speed = profile->speed_start +
                    (int)((profile->speed_max - profile->speed_start) * ratio);
        if (speed < 1) speed = 1;
        periods[i] = 1000000 / speed;  // 周期 us
    }

    /* 匀速阶段 */
    for (int i = 0; i < const_steps; i++) {
        periods[accel_steps + i] = 1000000 / profile->speed_max;
    }

    /* 减速阶段 */
    for (int i = 0; i < decel_steps; i++) {
        float ratio = (float)i / decel_steps;
        int speed = profile->speed_max -
                    (int)((profile->speed_max - profile->speed_start) * ratio);
        if (speed < profile->speed_start) speed = profile->speed_start;
        if (speed < 1) speed = 1;
        periods[accel_steps + const_steps + i] = 1000000 / speed;
    }

    /* 创建简单编码器 */
    rmt_encoder_handle_t encoder = NULL;
    rmt_simple_encoder_config_t encoder_config = {
        .callback = stepper_encoder_callback,
        .arg = NULL,
        .min_chunk_size = 64,  // 最小块大小
    };

    esp_err_t err = rmt_new_simple_encoder(&encoder_config, &encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create encoder: %s", esp_err_to_name(err));
        free(periods);
        goto cleanup;
    }

    /* 发送脉冲序列 */
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  // 不循环
    };

    err = rmt_transmit(motor->rmt_chan, encoder, periods,
                       profile->steps_total * sizeof(uint32_t), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit: %s", esp_err_to_name(err));
        rmt_del_encoder(encoder);
        free(periods);
        goto cleanup;
    }

    /* 等待传输完成或停止请求 */
    while (!motor->stop_request) {
        err = rmt_tx_wait_all_done(motor->rmt_chan, 100);  // 100ms 超时
        if (err == ESP_OK) {
            break;  // 传输完成
        }
        /* 超时继续循环，检查 stop_request */
    }

    /* 如果有停止请求，禁用 RMT 通道立即停止 */
    if (motor->stop_request) {
        rmt_disable(motor->rmt_chan);
        rmt_enable(motor->rmt_chan);
        ESP_LOGI(TAG, "Move stopped by request");
    }

    /* 清理 */
    rmt_del_encoder(encoder);
    free(periods);

    ESP_LOGI(TAG, "Move complete or stopped");

cleanup:
    motor->is_moving = false;
    motor->stop_request = false;

    free(params);
    motor->task_handle = NULL;
    vTaskDelete(NULL);
}

/* stepper_init 实现 */
esp_err_t tool_stepper_init_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 step_pin */
    cJSON *step_item = cJSON_GetObjectItem(root, "step_pin");
    if (!step_item || !cJSON_IsNumber(step_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'step_pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin_step = step_item->valueint;

    /* 解析 dir_pin */
    cJSON *dir_item = cJSON_GetObjectItem(root, "dir_pin");
    if (!dir_item || !cJSON_IsNumber(dir_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'dir_pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin_dir = dir_item->valueint;

    /* 解析 enable_pin */
    cJSON *en_item = cJSON_GetObjectItem(root, "enable_pin");
    if (!en_item || !cJSON_IsNumber(en_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'enable_pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin_enable = en_item->valueint;

    /* 解析 microstep（可选，默认16） */
    cJSON *ms_item = cJSON_GetObjectItem(root, "microstep");
    int microstep = 16;
    if (ms_item && cJSON_IsNumber(ms_item)) {
        microstep = ms_item->valueint;
    }
    /* 验证微步值 */
    if (microstep != 1 && microstep != 2 && microstep != 4 &&
        microstep != 8 && microstep != 16 && microstep != 32) {
        snprintf(output, output_size, "Error: microstep must be 1, 2, 4, 8, 16, or 32");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查引脚可用性 */
    if (!is_gpio_available(pin_step) || !is_gpio_available(pin_dir) || !is_gpio_available(pin_enable)) {
        snprintf(output, output_size, "Error: one or more pins are not available");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查是否已初始化 */
    stepper_motor_t *existing = find_motor(pin_step);
    if (existing) {
        /* 重新初始化：停止当前任务 */
        if (existing->is_moving) {
            existing->stop_request = true;
            vTaskDelay(pdMS_TO_TICKS(100));  // 等待停止
        }
        free_motor(existing);
    }

    /* 分配新实例 */
    stepper_motor_t *motor = allocate_motor();
    if (!motor) {
        snprintf(output, output_size, "Error: no free motor slot available (max %d)", STEPPER_MAX_MOTORS);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    /* 创建 RMT TX 通道 - 用于生成 STEP 脉冲 */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin_step,
        .mem_block_symbols = 256,  // 更大的内存块，支持更多脉冲缓冲
        .resolution_hz = STEPPER_RMT_RESOLUTION_HZ,  // 1MHz = 1us
        .trans_queue_depth = 2,
        .flags = {
            .invert_out = false,
            .with_dma = false,  // 步进电机脉冲量不大，不需要 DMA
        },
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &motor->rmt_chan);
    if (err != ESP_OK) {
        free_motor(motor);
        snprintf(output, output_size, "Error: failed to create RMT channel (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 启动 RMT 通道 */
    err = rmt_enable(motor->rmt_chan);
    if (err != ESP_OK) {
        free_motor(motor);
        snprintf(output, output_size, "Error: failed to enable RMT channel (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 配置 DIR 和 ENABLE GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_dir) | (1ULL << pin_enable),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        free_motor(motor);
        snprintf(output, output_size, "Error: failed to configure GPIO (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 初始化引脚状态 */
    gpio_set_level(pin_dir, 0);
    gpio_set_level(pin_enable, 1);  // 默认禁用（高电平）

    /* 保存配置 */
    motor->pin_step = pin_step;
    motor->pin_dir = pin_dir;
    motor->pin_enable = pin_enable;
    motor->microstep = microstep;
    motor->is_moving = false;
    motor->stop_request = false;
    motor->task_handle = NULL;

    snprintf(output, output_size, "OK: Stepper initialized on STEP=GPIO%d, DIR=GPIO%d, EN=GPIO%d, microstep=%d",
             pin_step, pin_dir, pin_enable, microstep);
    ESP_LOGI(TAG, "stepper_init: step=%d, dir=%d, en=%d, microstep=%d", pin_step, pin_dir, pin_enable, microstep);
    cJSON_Delete(root);
    return ESP_OK;
}

/* stepper_move 实现 */
esp_err_t tool_stepper_move_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 step_pin */
    cJSON *step_item = cJSON_GetObjectItem(root, "step_pin");
    if (!step_item || !cJSON_IsNumber(step_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'step_pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin_step = step_item->valueint;

    /* 查找电机实例 */
    stepper_motor_t *motor = find_motor(pin_step);
    if (!motor) {
        snprintf(output, output_size, "Error: stepper not initialized on STEP=GPIO%d, call stepper_init first", pin_step);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    /* 检查是否正在转动 */
    if (motor->is_moving) {
        snprintf(output, output_size, "Error: motor is already moving, call stepper_stop first");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    /* 解析 steps */
    cJSON *steps_item = cJSON_GetObjectItem(root, "steps");
    if (!steps_item || !cJSON_IsNumber(steps_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'steps' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int steps = steps_item->valueint;
    if (steps <= 0 || steps > 100000) {
        snprintf(output, output_size, "Error: steps must be 1-100000");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 direction */
    const char *dir_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "direction"));
    if (!dir_str) {
        snprintf(output, output_size, "Error: missing 'direction' field (use 'cw' or 'ccw')");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    bool direction;
    if (strcmp(dir_str, "cw") == 0) {
        direction = true;
    } else if (strcmp(dir_str, "ccw") == 0) {
        direction = false;
    } else {
        snprintf(output, output_size, "Error: direction must be 'cw' or 'ccw'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 speed_max（可选） */
    cJSON *speed_max_item = cJSON_GetObjectItem(root, "speed_max");
    int speed_max = DEFAULT_SPEED_MAX;
    if (speed_max_item && cJSON_IsNumber(speed_max_item)) {
        speed_max = speed_max_item->valueint;
    }
    if (speed_max < 100 || speed_max > 5000) {
        snprintf(output, output_size, "Error: speed_max must be 100-5000 Hz");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 speed_start（可选） */
    cJSON *speed_start_item = cJSON_GetObjectItem(root, "speed_start");
    int speed_start = DEFAULT_SPEED_START;
    if (speed_start_item && cJSON_IsNumber(speed_start_item)) {
        speed_start = speed_start_item->valueint;
    }
    if (speed_start < 100 || speed_start >= speed_max) {
        snprintf(output, output_size, "Error: speed_start must be 100-speed_max");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 accel_ratio（可选） */
    cJSON *accel_item = cJSON_GetObjectItem(root, "accel_ratio");
    float accel_ratio = DEFAULT_ACCEL_RATIO;
    if (accel_item && cJSON_IsNumber(accel_item)) {
        accel_ratio = accel_item->valuedouble;
    }
    if (accel_ratio < 0.0 || accel_ratio > 0.5) {
        snprintf(output, output_size, "Error: accel_ratio must be 0.0-0.5");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 准备任务参数 */
    move_task_params_t *params = malloc(sizeof(move_task_params_t));
    if (!params) {
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    params->motor = motor;
    params->profile.steps_total = steps;
    params->profile.speed_start = speed_start;
    params->profile.speed_max = speed_max;
    params->profile.accel_ratio = accel_ratio;
    params->direction = direction;

    /* 标记为运动中 */
    motor->is_moving = true;
    motor->stop_request = false;

    /* 创建后台任务执行转动 */
    BaseType_t ret = xTaskCreate(stepper_move_task, "stepper_move", 4096, params, 5, &motor->task_handle);
    if (ret != pdPASS) {
        motor->is_moving = false;
        free(params);
        snprintf(output, output_size, "Error: failed to create move task");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "OK: Moving %d steps %s at %d Hz (start=%d, accel=%.0f%%)",
             steps, direction ? "CW" : "CCW", speed_max, speed_start, accel_ratio * 100);
    ESP_LOGI(TAG, "stepper_move: pin=%d steps=%d dir=%s speed=%d", pin_step, steps, dir_str, speed_max);
    cJSON_Delete(root);
    return ESP_OK;
}

/* stepper_stop 实现 */
esp_err_t tool_stepper_stop_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* 解析 step_pin */
    cJSON *step_item = cJSON_GetObjectItem(root, "step_pin");
    if (!step_item || !cJSON_IsNumber(step_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'step_pin' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int pin_step = step_item->valueint;

    /* 查找电机实例 */
    stepper_motor_t *motor = find_motor(pin_step);
    if (!motor) {
        snprintf(output, output_size, "Error: stepper not initialized on STEP=GPIO%d", pin_step);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    /* 检查是否正在转动 */
    if (!motor->is_moving) {
        snprintf(output, output_size, "OK: Motor is already stopped");
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* 请求停止 */
    motor->stop_request = true;

    snprintf(output, output_size, "OK: Stop requested for motor on STEP=GPIO%d", pin_step);
    ESP_LOGI(TAG, "stepper_stop: pin=%d", pin_step);
    cJSON_Delete(root);
    return ESP_OK;
}
