# Tool Sequence

Execute multi-step hardware timing sequences using tool_sequence and delay_execute.

## When to use

When the user wants GPIO timing patterns: LED blinks, beep sequences, or sensor triggers with precise delays.
Also for grouping related operations that must complete together.

## Key tools

- `tool_sequence`: Execute multiple tools in order (max 20 steps)
- `delay_execute`: Wait N milliseconds, then execute a tool

## Common patterns

### Simple blink (on 500ms, then off)

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

### Multi-stage (on 50ms, off 100ms, on 300ms, off)

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

### With GPIO setup

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

## Important notes

- Use `tool_args` (not `args`) inside delay_execute to pass arguments to the target tool
- delay_execute blocks: the sequence waits for each delay to complete
- Fast-fail: if any step fails, the sequence stops immediately
- Max 20 steps per sequence
- Cannot nest: tool_sequence cannot call tool_sequence

## Allowed tools in sequences

gpio_config, gpio_set, gpio_read, gpio_pwm, delay_execute, get_current_time, write_file
