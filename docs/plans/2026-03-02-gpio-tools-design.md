# GPIO 工具设计文档

## 概述

为 MimiClaw ESP32-S3 固件添加 GPIO 控制工具，支持基础 GPIO 操作和 PWM 输出。

**设计原则**：
- 通用 PWM 使用 LEDC 驱动（简单、够用）
- 复杂电机控制后续使用 MCPWM 驱动（专门工具）
- 遵循现有工具架构模式

---

## 工具集合

### 1. gpio_config - 配置引脚模式

配置 GPIO 引脚的输入/输出模式，可选上下拉电阻。

**JSON Schema**:
```json
{
  "type": "object",
  "properties": {
    "pin": {"type": "integer", "description": "GPIO pin number (0-48)", "minimum": 0, "maximum": 48},
    "mode": {"type": "string", "description": "Pin mode", "enum": ["input", "output", "input_output"]},
    "pull_up": {"type": "boolean", "description": "Enable internal pull-up resistor (default: false)"},
    "pull_down": {"type": "boolean", "description": "Enable internal pull-down resistor (default: false)"}
  },
  "required": ["pin", "mode"]
}
```

**返回值**:
- 成功: `"OK: GPIO{pin} configured as {mode}"`
- 失败: `"Error: {reason}"`

---

### 2. gpio_set - 设置输出电平

设置 GPIO 输出为高电平或低电平。

**JSON Schema**:
```json
{
  "type": "object",
  "properties": {
    "pin": {"type": "integer", "description": "GPIO pin number (0-48)", "minimum": 0, "maximum": 48},
    "value": {"type": "integer", "description": "Output level", "enum": [0, 1]}
  },
  "required": ["pin", "value"]
}
```

**返回值**:
- 成功: `"OK: GPIO{pin} set to {high/low}"`
- 失败: `"Error: {reason}"`

---

### 3. gpio_read - 读取输入状态

读取 GPIO 输入电平状态。

**JSON Schema**:
```json
{
  "type": "object",
  "properties": {
    "pin": {"type": "integer", "description": "GPIO pin number (0-48)", "minimum": 0, "maximum": 48}
  },
  "required": ["pin"]
}
```

**返回值**:
- 成功: `"GPIO{pin} = {0/1} (low/high)"`
- 失败: `"Error: {reason}"`

---

### 4. gpio_pwm - PWM 输出控制

使用 LEDC 外设产生 PWM 信号。

**JSON Schema**:
```json
{
  "type": "object",
  "properties": {
    "pin": {"type": "integer", "description": "GPIO pin number (0-48)", "minimum": 0, "maximum": 48},
    "frequency": {"type": "integer", "description": "PWM frequency in Hz (1-40000000)", "minimum": 1, "maximum": 40000000},
    "duty": {"type": "integer", "description": "Duty cycle percentage (0-100)", "minimum": 0, "maximum": 100},
    "resolution_bits": {"type": "integer", "description": "PWM resolution in bits (1-14, default: 8)", "minimum": 1, "maximum": 14},
    "enable": {"type": "boolean", "description": "Enable or disable PWM output (default: true)"}
  },
  "required": ["pin"]
}
```

**行为**:
- `enable=true`（默认）: 启动 PWM，需要 `frequency` 和 `duty`
- `enable=false`: 停止 PWM，释放 LEDC 通道
- 修改现有 PWM: 保持 `enable=true`，提供新的 `frequency`/`duty`

**返回值**:
- 成功启动: `"OK: PWM started on GPIO{pin} @ {freq}Hz {duty}%"`
- 成功停止: `"OK: PWM stopped on GPIO{pin}"`
- 失败: `"Error: {reason}"`

---

## 可用引脚范围

**允许使用**（保守默认值）：
- GPIO 0-18
- GPIO 33-42
- GPIO 45-48

**限制引脚**（自动拒绝）：
- GPIO 19-24（SPI Flash）
- GPIO 26-32（Flash/PSRAM）
- GPIO 43-44（USB-JTAG）

---

## 错误处理

| 错误场景 | 返回值示例 |
|---------|-----------|
| 引脚超出可用范围 | `Error: GPIO{pin} is not available (restricted: flash/usb pins)` |
| 引脚号无效 | `Error: GPIO{pin} is invalid (valid: 0-48)` |
| 参数缺失 | `Error: missing 'mode' field` |
| 无效参数值 | `Error: invalid mode '{xxx}' (valid: input, output, input_output)` |
| PWM 资源耗尽 | `Error: no free LEDC channel available` |

---

## 实现细节

### 文件结构
```
main/tools/
├── tool_gpio.h      # 函数声明
├── tool_gpio.c      # 实现
```

### 依赖库
- `driver/gpio.h` - GPIO 驱动
- `driver/ledc.h` - LEDC PWM 驱动

### LEDC 通道管理
- 使用 `LEDC_LOW_SPEED_MODE`
- 自动分配可用通道（共 8 个）
- PWM 停止时释放通道

### 内部状态
- 静态数组跟踪引脚配置状态
- PWM 通道映射表（pin → channel）

---

## 使用示例

### 控制 LED
```
1. gpio_config: {"pin": 2, "mode": "output"}
2. gpio_set: {"pin": 2, "value": 1}    # LED 亮
3. gpio_set: {"pin": 2, "value": 0}    # LED 灭
```

### PWM 调光
```
1. gpio_pwm: {"pin": 4, "frequency": 1000, "duty": 50}   # 50% 亮度
2. gpio_pwm: {"pin": 4, "duty": 25}                      # 改为 25%
3. gpio_pwm: {"pin": 4, "enable": false}                 # 关闭 PWM
```

### 读取按钮
```
1. gpio_config: {"pin": 5, "mode": "input", "pull_up": true}
2. gpio_read: {"pin": 5}  # 返回 0 (按下) 或 1 (松开)
```

### 舵机控制
```
1. gpio_pwm: {"pin": 18, "frequency": 50, "duty": 7.5}   # 中间位置 (1.5ms)
2. gpio_pwm: {"pin": 18, "duty": 5}                       # 左转 (1ms)
3. gpio_pwm: {"pin": 18, "duty": 10}                      # 右转 (2ms)
```

---

## 后续扩展

复杂电机控制将使用 MCPWM 驱动，单独实现：

- `motor_dc` - H桥直流电机（互补 PWM + 死区）
- `motor_bldc` - 三相无刷电机
- `motor_stepper` - 步进电机

这些工具将位于 `tool_motor.h/c`，使用 `driver/mcpwm.h`。

---

## 决策记录

| 决策 | 选择 | 原因 |
|------|------|------|
| PWM 驱动 | LEDC | 简单、通用，适合 LED/舵机/简单电机 |
| 分辨率默认 | 8-bit | 256 级精度适合大多数场景，可配置 1-14bit |
| 工具数量 | 4 个 | 单一职责，符合现有工具风格 |
| 引脚限制 | 保守模式 | 排除 Flash/USB 引脚，避免硬件冲突 |
| 电机高级功能 | 后续单独工具 | MCPWM 复杂度高，与通用 GPIO 分层更清晰 |
