# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 交流约定

除非是必须使用英文情况(比如: 术语, 代码, 避免歧义等), 否则使用简体中文进行交流.

## 网络环境限制

- Claude Code 内置的 WebSearch 工具不可用, 必须考虑其他方案

## 项目概述

MimiClaw 是一个运行在 ESP32-S3 单片机上的 AI 助手固件，使用纯 C 语言编写，基于 FreeRTOS 和 ESP-IDF 框架。它通过 Telegram Bot 或 WebSocket 与用户交互，调用 Anthropic Claude 或 OpenAI API 进行对话，支持工具调用（ReAct 模式）。

硬件要求：ESP32-S3 开发板（16MB Flash + 8MB PSRAM），如 Xiaozhi AI 板。

## 常用命令

### 构建和烧录

```bash
# 首次设置目标芯片（只需执行一次）
idf.py set-target esp32s3

# 完整重新构建（修改 mimi_secrets.h 后必须执行）
idf.py fullclean && idf.py build

# 查找串口
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 烧录并监控（使用 USB/JTAG 端口）
idf.py -p /dev/ttyACM0 flash monitor

# 仅监控串口输出（使用 UART/COM 端口）
idf.py -p /dev/ttyUSB0 monitor

# 退出监控: Ctrl+]
```

### 配置

```bash
# 复制配置文件模板
cp main/mimi_secrets.h.example main/mimi_secrets.h

# 编辑后必须执行完整重建
idf.py fullclean && idf.py build
```

## 架构概览

### 双核任务分配

- **Core 0**: I/O 处理（网络、串口、WiFi）
  - `tg_poll`: Telegram 长轮询
  - `outbound`: 响应分发
  - `serial_cli`: 串口 REPL
  - WebSocket 服务器

- **Core 1**: AI 处理
  - `agent_loop`: ReAct 循环（LLM 调用 + 工具执行）

### 消息总线

使用两个 FreeRTOS 队列传递 `mimi_msg_t` 结构：

- **Inbound Queue**: 外部输入 → Agent Loop（深度 16）
- **Outbound Queue**: Agent Loop → 输出分发（深度 16）

### Agent 处理流程

```
1. 接收消息 → 2. 加载会话历史 → 3. 构建系统提示词
→ 4. ReAct 循环（最多 10 轮）→ 5. 保存会话 → 6. 发送响应
```

系统提示词由以下部分组成：

- `SOUL.md`: AI 人格定义
- `USER.md`: 用户信息
- `MEMORY.md`: 长期记忆
- `YYYY-MM-DD.md`: 最近 3 天的笔记

### 工具调用

支持的工具（`tools/` 目录）：

- `web_search`: Brave Search API 网页搜索
- `get_current_time`: 获取当前时间
- `cron_add/cron_list/cron_remove`: 定时任务管理
- `file_read/file_write/file_list`: SPIFFS 文件操作

### 存储布局（SPIFFS）

SPIFFS 挂载于 `/spiffs`，总共 12MB：

- `/spiffs/config/`: SOUL.md, USER.md
- `/spiffs/memory/`: MEMORY.md, 2026-02-28.md（每日笔记）
- `/spiffs/sessions/`: tg_12345.jsonl（会话历史，JSONL 格式）
- `/spiffs/cron.json`: 定时任务配置
- `/spiffs/HEARTBEAT.md`: 心跳任务列表

### 配置系统（双层）

1. **编译时配置**: `mimi_secrets.h`（最高优先级）
2. **运行时配置**: NVS 存储，通过串口 CLI 设置

运行时配置命令（UART/COM 端口）：

```
mimi> wifi_set MySSID MyPassword
mimi> set_api_key sk-ant-api03-...
mimi> set_model_provider anthropic|openai
mimi> set_proxy 127.0.0.1 7897
mimi> config_show
mimi> config_reset
```

## 关键文件说明

| 文件                      | 说明                             |
| ------------------------- | -------------------------------- |
| `main/mimi.c`             | 入口点，启动序列，任务创建       |
| `main/mimi_config.h`      | 所有编译时常量配置               |
| `main/mimi_secrets.h`     | 私密配置（WiFi、API Key、Token） |
| `main/agent/agent_loop.c` | ReAct 循环实现                   |
| `main/llm/llm_proxy.c`    | LLM API 调用（Anthropic/OpenAI） |
| `main/bus/message_bus.c`  | 消息队列实现                     |

## 开发注意事项

### 硬件双端口说明

ESP32-S3 开发板通常有两个 USB-C 端口：

- **USB 端口**（JTAG）: 用于 `idf.py flash` 烧录
- **COM 端口**（UART）: 用于 REPL 串口交互

REPL CLI 只能在 UART/COM 端口上使用。

### 内存管理

- 大缓冲区（32KB+）使用 PSRAM: `heap_caps_calloc(size, MALLOC_CAP_SPIRAM)`
- TLS 连接占用约 120KB PSRAM
- 任务栈分配在内部 SRAM

## 参考文档

目前项目正在快速发展, 文档具有滞后性, 必须结合实际代码进行判断

- `docs/ARCHITECTURE.md`: 详细架构设计
- `docs/TODO.md`: 功能缺口追踪（对比 Nanobot）
- `README.md`: 用户指南和快速开始
