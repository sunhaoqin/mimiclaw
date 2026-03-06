#include "tool_ws2812.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "tool_ws2812";

/* WS2812 时序参数 (单位: 滴答, 基于 80MHz APB 时钟) */
#define WS2812_RMT_RESOLUTION_HZ    10000000  // 10MHz, 1 tick = 0.1us
#define WS2812_T0H_NS               400       // 0 bit 高电平时间
#define WS2812_T0L_NS               850       // 0 bit 低电平时间
#define WS2812_T1H_NS               800       // 1 bit 高电平时间
#define WS2812_T1L_NS               450       // 1 bit 低电平时间
#define WS2812_RESET_US             280       // Reset 信号低电平时间

/* 每个 RMT 通道支持的 LED 数量上限 */
#define WS2812_MAX_LEDS             256
#define WS2812_MAX_STRIPS           4

/* 单个 WS2812 条带状态 */
typedef struct {
    int pin;
    int num_leds;
    bool in_use;
    rmt_channel_handle_t rmt_chan;
    rmt_encoder_handle_t encoder;
    uint8_t *buffer;  // RGB 缓冲区
} ws2812_strip_t;

static ws2812_strip_t s_strips[WS2812_MAX_STRIPS] = {0};
static rmt_transmit_config_t s_tx_config = {
    .loop_count = 0,
};

/* 查找或分配条带 */
static ws2812_strip_t* find_strip(int pin)
{
    for (int i = 0; i < WS2812_MAX_STRIPS; i++) {
        if (s_strips[i].in_use && s_strips[i].pin == pin) {
            return &s_strips[i];
        }
    }
    return NULL;
}

static ws2812_strip_t* allocate_strip(void)
{
    for (int i = 0; i < WS2812_MAX_STRIPS; i++) {
        if (!s_strips[i].in_use) {
            s_strips[i].in_use = true;
            return &s_strips[i];
        }
    }
    return NULL;
}

static void free_strip(ws2812_strip_t *strip)
{
    if (strip->rmt_chan) {
        rmt_del_channel(strip->rmt_chan);
        strip->rmt_chan = NULL;
    }
    if (strip->encoder) {
        rmt_del_encoder(strip->encoder);
        strip->encoder = NULL;
    }
    if (strip->buffer) {
        free(strip->buffer);
        strip->buffer = NULL;
    }
    strip->in_use = false;
    strip->pin = -1;
    strip->num_leds = 0;
}

/* 检查 GPIO 是否可用 */
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

/* 创建 WS2812 编码器 - 使用 RMT 字节编码器 */
static esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *encoder)
{
    /* 使用字节编码器，每个字节变成 8 个 RMT 符号 (bits) */
    rmt_bytes_encoder_config_t bytes_enc_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = WS2812_T0H_NS / 100,  // 0.1us tick
            .level1 = 0,
            .duration1 = WS2812_T0L_NS / 100,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = WS2812_T1H_NS / 100,
            .level1 = 0,
            .duration1 = WS2812_T1L_NS / 100,
        },
        .flags = {
            .msb_first = 1,  // WS2812 要求 MSB 先发送
        },
    };

    return rmt_new_bytes_encoder(&bytes_enc_config, encoder);
}

/* ws2812_init 实现 */
esp_err_t tool_ws2812_init_execute(const char *input_json, char *output, size_t output_size)
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

    /* 解析 num_leds */
    cJSON *leds_item = cJSON_GetObjectItem(root, "num_leds");
    if (!leds_item || !cJSON_IsNumber(leds_item)) {
        snprintf(output, output_size, "Error: missing or invalid 'num_leds' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    int num_leds = leds_item->valueint;
    if (num_leds < 1 || num_leds > WS2812_MAX_LEDS) {
        snprintf(output, output_size, "Error: num_leds must be 1-%d", WS2812_MAX_LEDS);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查是否已初始化 */
    ws2812_strip_t *existing = find_strip(pin);
    if (existing) {
        /* 重新初始化 */
        free_strip(existing);
    }

    /* 分配新的条带 */
    ws2812_strip_t *strip = allocate_strip();
    if (!strip) {
        snprintf(output, output_size, "Error: no free WS2812 slot available (max %d strips)", WS2812_MAX_STRIPS);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    /* 创建 RMT TX 通道 */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin,
        .mem_block_symbols = 64,  // 每个块 64 个符号
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
        },
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &strip->rmt_chan);
    if (err != ESP_OK) {
        free_strip(strip);
        snprintf(output, output_size, "Error: failed to create RMT channel (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 创建编码器 */
    err = create_ws2812_encoder(&strip->encoder);
    if (err != ESP_OK) {
        free_strip(strip);
        snprintf(output, output_size, "Error: failed to create encoder (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 启动通道 */
    err = rmt_enable(strip->rmt_chan);
    if (err != ESP_OK) {
        free_strip(strip);
        snprintf(output, output_size, "Error: failed to enable RMT channel (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 分配颜色缓冲区 */
    strip->buffer = calloc(num_leds * 3, 1);
    if (!strip->buffer) {
        free_strip(strip);
        snprintf(output, output_size, "Error: failed to allocate buffer");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    strip->pin = pin;
    strip->num_leds = num_leds;

    snprintf(output, output_size, "OK: WS2812 initialized on GPIO%d with %d LEDs", pin, num_leds);
    ESP_LOGI(TAG, "ws2812_init: pin=%d num_leds=%d", pin, num_leds);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 辅助函数: 解析颜色 */
static bool parse_color(cJSON *color_obj, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!color_obj || !cJSON_IsObject(color_obj)) {
        return false;
    }

    cJSON *r_item = cJSON_GetObjectItem(color_obj, "r");
    cJSON *g_item = cJSON_GetObjectItem(color_obj, "g");
    cJSON *b_item = cJSON_GetObjectItem(color_obj, "b");

    if (!r_item || !cJSON_IsNumber(r_item) ||
        !g_item || !cJSON_IsNumber(g_item) ||
        !b_item || !cJSON_IsNumber(b_item)) {
        return false;
    }

    *r = (uint8_t)r_item->valueint;
    *g = (uint8_t)g_item->valueint;
    *b = (uint8_t)b_item->valueint;
    return true;
}

/* 辅助函数: 发送缓冲区到 LED */
static esp_err_t refresh_strip(ws2812_strip_t *strip)
{
    /* 发送数据 */
    esp_err_t err = rmt_transmit(strip->rmt_chan, strip->encoder,
                                  strip->buffer, strip->num_leds * 3,
                                  &s_tx_config);
    if (err != ESP_OK) {
        return err;
    }

    /* 等待传输完成 */
    err = rmt_tx_wait_all_done(strip->rmt_chan, 1000);
    if (err != ESP_OK) {
        return err;
    }

    /* 发送 reset 信号 - bytes_encoder 结束后自动保持低电平，
       延时 300us 确保 LED 锁存数据 */
    ets_delay_us(WS2812_RESET_US + 20);

    return ESP_OK;
}

/* ws2812_set 实现 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size)
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

    ws2812_strip_t *strip = find_strip(pin);
    if (!strip) {
        snprintf(output, output_size, "Error: WS2812 not initialized on GPIO%d, call ws2812_init first", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    /* 解析 index */
    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    int idx = -1;  // -1 表示所有 LED
    if (idx_item) {
        if (!cJSON_IsNumber(idx_item)) {
            snprintf(output, output_size, "Error: 'index' must be a number");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        idx = idx_item->valueint;
        if (idx < 0 || idx >= strip->num_leds) {
            snprintf(output, output_size, "Error: index %d out of range (0-%d)", idx, strip->num_leds - 1);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* 解析颜色 */
    cJSON *color_obj = cJSON_GetObjectItem(root, "color");
    uint8_t r, g, b;
    if (!parse_color(color_obj, &r, &g, &b)) {
        snprintf(output, output_size, "Error: missing or invalid 'color' field (expected {r,g,b})");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 设置颜色 */
    if (idx >= 0) {
        /* 单个 LED */
        strip->buffer[idx * 3 + 0] = g;  // WS2812 顺序是 GRB
        strip->buffer[idx * 3 + 1] = r;
        strip->buffer[idx * 3 + 2] = b;
    } else {
        /* 所有 LED */
        for (int i = 0; i < strip->num_leds; i++) {
            strip->buffer[i * 3 + 0] = g;
            strip->buffer[i * 3 + 1] = r;
            strip->buffer[i * 3 + 2] = b;
        }
    }

    /* 刷新显示 */
    esp_err_t err = refresh_strip(strip);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to update LEDs (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    if (idx >= 0) {
        snprintf(output, output_size, "OK: LED %d set to RGB(%d,%d,%d)", idx, r, g, b);
        ESP_LOGI(TAG, "ws2812_set: pin=%d idx=%d color=%d,%d,%d", pin, idx, r, g, b);
    } else {
        snprintf(output, output_size, "OK: All %d LEDs set to RGB(%d,%d,%d)", strip->num_leds, r, g, b);
        ESP_LOGI(TAG, "ws2812_set: pin=%d all color=%d,%d,%d", pin, r, g, b);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* ws2812_clear 实现 */
esp_err_t tool_ws2812_clear_execute(const char *input_json, char *output, size_t output_size)
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

    ws2812_strip_t *strip = find_strip(pin);
    if (!strip) {
        snprintf(output, output_size, "Error: WS2812 not initialized on GPIO%d", pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    /* 清零缓冲区 */
    memset(strip->buffer, 0, strip->num_leds * 3);

    /* 刷新显示 */
    esp_err_t err = refresh_strip(strip);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to clear LEDs (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: All %d LEDs cleared on GPIO%d", strip->num_leds, pin);
    ESP_LOGI(TAG, "ws2812_clear: pin=%d num_leds=%d", pin, strip->num_leds);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ws2812_fill 实现 */
esp_err_t tool_ws2812_fill_execute(const char *input_json, char *output, size_t output_size)
{
    /* fill 和 set (无index) 功能相同，直接调用 set */
    return tool_ws2812_set_execute(input_json, output, output_size);
}
