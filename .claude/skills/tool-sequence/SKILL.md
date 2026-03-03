---
name: tool-sequence
description: Use when constructing multi-step tool sequences with precise timing, especially GPIO blink patterns or sensor polling loops
---

# Tool Sequence

## Overview

Execute multiple tools in order with `tool_sequence`. Combines with `delay_execute` for hardware timing sequences.

## When to Use

- **GPIO timing patterns**: LED blinks, servo sequences, sensor triggers
- **Grouped operations**: Related steps that succeed/fail together
- **Single-turn completion**: Avoid multiple agent rounds for simple sequences

**NOT for**: User confirmation between steps, conditional logic, or independent operations.

## Quick Reference

| Pattern | Structure |
|---------|-----------|
| Simple blink | `gpio_set` → `delay_execute` → `gpio_set` |
| Multi-stage | `gpio_set` → `delay_execute` → `gpio_set` → `delay_execute`... |
| With setup | `gpio_config` → `gpio_set` → `delay_execute` → `gpio_set` |

## Common Patterns

### Pattern 1: Simple Blink (on 500ms, off)

```json
{
  "name": "tool_sequence",
  "arguments": {
    "steps": [
      {"tool": "gpio_set", "args": {"pin": 12, "value": 1}},
      {"tool": "delay_execute", "args": {"delay_ms": 500, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 0}}}
    ]
  }
}
```

### Pattern 2: Multi-Stage (on 50ms, off 100ms, on 300ms, off)

```json
{
  "name": "tool_sequence",
  "arguments": {
    "steps": [
      {"tool": "gpio_set", "args": {"pin": 12, "value": 1}},
      {"tool": "delay_execute", "args": {"delay_ms": 50, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 0}}},
      {"tool": "delay_execute", "args": {"delay_ms": 100, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 1}}},
      {"tool": "delay_execute", "args": {"delay_ms": 300, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 0}}}
    ]
  }
}
```

### Pattern 3: With GPIO Setup

```json
{
  "name": "tool_sequence",
  "arguments": {
    "steps": [
      {"tool": "gpio_config", "args": {"pin": 12, "mode": "output"}},
      {"tool": "gpio_set", "args": {"pin": 12, "value": 1}},
      {"tool": "delay_execute", "args": {"delay_ms": 3000, "tool_name": "gpio_set", "tool_args": {"pin": 12, "value": 0}}}
    ]
  }
}
```

## Key Parameters

**`delay_execute` structure:**
- `delay_ms`: Wait time before executing
- `tool_name`: Target tool to run after delay
- `tool_args`: Arguments for target tool (use `tool_args`, not `args`)

**Common mistake**: Using `args` instead of `tool_args` inside `delay_execute`.

## Allowed Tools in Sequences

- `gpio_config`, `gpio_set`, `gpio_read`, `gpio_pwm`
- `delay_execute`
- `get_current_time`, `write_file`, `read_file`

**Not allowed**: `tool_sequence` (no nesting), `cron_add`

## Error Handling

- **Fast-fail**: First error stops the sequence
- **Pre-check**: Include `gpio_config` first if pin state unknown
- **Max 20 steps**: Split long sequences or use `cron_add`

## Response Format

Success returns:
```json
{
  "status": "success",
  "steps_executed": 4,
  "duration_ms": 450,
  "log_file": "/spiffs/logs/tool_sequence_20260303_143022.jsonl"
}
```

Failure returns failed step index and error message.
