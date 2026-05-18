---
{
  "name": "light_animation",
  "description": "Animate the onboard RGB LED with looping color sequences (rainbow or cycle). Uses async Lua so the LED keeps animating after the call returns, and stops cleanly when the user asks.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Light Animation

Use this skill when the user asks the onboard LED to animate, cycle colors, run a rainbow, loop colors, do a "demo mode", or keep changing color over time.

Do **not** use this skill for setting a single static color. For that use the `light_switch` skill.

## Prerequisites

Read `board_hardware_info` first to confirm the LED's GPIO (currently GPIO48 on this board). Pass it as `io` if it differs from the script default.

## Available scripts

- `/fatfs/skills/light_animation/scripts/led_strip_sequence.lua`

## Calling rules

**Always invoke this script with `lua_run_script_async`, never with the sync `lua_run_script`.** The sync version would block the agent loop until the animation ends.

Required async parameters:

```json
{
  "path": "/fatfs/skills/light_animation/scripts/led_strip_sequence.lua",
  "name": "led_animation",
  "exclusive": "led_strip",
  "replace": true,
  "timeout_ms": 0,
  "args": { ... }
}
```

- `name: "led_animation"` so the user (and you) can later call `lua_stop_async_job name=led_animation`.
- `exclusive: "led_strip"` so any other LED job (light_switch, another animation) is blocked or replaced cleanly.
- `replace: true` lets a new animation request swap out an already-running one without an error.
- `timeout_ms: 0` runs until cancelled. Pass a positive value (or set `args.duration_ms`) for a hard cap.

## Stopping

If the user asks to stop, turn off, pause, end, or apagar the animation, call `lua_stop_async_job` in the same turn:

```json
{ "name": "led_animation", "wait_ms": 2000 }
```

The script's cleanup handler turns the LED off by default (`off_when_done=true`), so after a successful stop the LED is dark.

Do **not** claim the animation stopped based on context. Always confirm via the stop tool.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "io":            { "type": "integer", "default": 48, "minimum": 0 },
    "led_count":     { "type": "integer", "default": 1,  "minimum": 1 },
    "brightness":    { "type": "integer", "default": 255, "minimum": 0, "maximum": 255 },
    "step_ms":       { "type": "integer", "default": 50, "minimum": 10 },
    "duration_ms":   { "type": "integer", "default": 0,  "minimum": 0,
                       "description": "0 = run until cancelled. >0 = stop after N ms even if no cancellation." },
    "off_when_done": { "type": "boolean", "default": true,
                       "description": "If true, the LED is turned off when the loop exits or is cancelled." },
    "mode":          { "type": "string",  "enum": ["rainbow", "cycle"], "default": "rainbow" },
    "colors":        {
      "type": "array",
      "description": "Used only when mode='cycle'. List of {r,g,b} bytes. Defaults to a 6-color palette.",
      "items": {
        "type": "object",
        "properties": {
          "r": { "type": "integer", "minimum": 0, "maximum": 255 },
          "g": { "type": "integer", "minimum": 0, "maximum": 255 },
          "b": { "type": "integer", "minimum": 0, "maximum": 255 }
        }
      }
    }
  }
}
```

## Examples

Rainbow forever until the user says stop:

```json
{
  "path": "/fatfs/skills/light_animation/scripts/led_strip_sequence.lua",
  "name": "led_animation",
  "exclusive": "led_strip",
  "replace": true,
  "timeout_ms": 0,
  "args": { "mode": "rainbow" }
}
```

Cycle red/green/blue every 300 ms for 10 seconds:

```json
{
  "path": "/fatfs/skills/light_animation/scripts/led_strip_sequence.lua",
  "name": "led_animation",
  "exclusive": "led_strip",
  "replace": true,
  "timeout_ms": 0,
  "args": {
    "mode": "cycle",
    "step_ms": 300,
    "duration_ms": 10000,
    "colors": [
      { "r": 255, "g": 0,   "b": 0   },
      { "r": 0,   "g": 255, "b": 0   },
      { "r": 0,   "g": 0,   "b": 255 }
    ]
  }
}
```

Stop:

```json
{ "name": "led_animation", "wait_ms": 2000 }
```
