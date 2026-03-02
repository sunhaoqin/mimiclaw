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
