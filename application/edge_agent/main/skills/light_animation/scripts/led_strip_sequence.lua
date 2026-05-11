-- --------------------------------------------------------------
-- Cycle a WS2812 LED through colors.
--   mode = "rainbow": HSV sweep (hue increments every step)
--   mode = "cycle":   iterate through `colors` array (or built-in palette)
--
-- Designed for `lua_run_script_async`. Runs until cancelled via
-- `lua_stop_async_job`. delay.delay_ms() is the cooperative cancel point.
--
-- Optional safety cap: pass `duration_ms > 0` to auto-stop after N ms.
-- --------------------------------------------------------------

local arg_schema = require("arg_schema")
local led_strip = require("led_strip")
local delay = require("delay")

local DEFAULT_IO = 48
local DEFAULT_LED_COUNT = 1
local DEFAULT_BRIGHTNESS = 255
local DEFAULT_STEP_MS = 50
local DEFAULT_DURATION_MS = 0           -- 0 = infinite, stop via lua_stop_async_job
local DEFAULT_OFF_WHEN_DONE = true      -- safer default: leave LED off after stop
local HSV_HUE_STEP = 6                  -- degrees per step in rainbow mode

local ARG_SCHEMA = {
  io          = arg_schema.int ({ default = DEFAULT_IO,            min = 0 }),
  led_count   = arg_schema.int ({ default = DEFAULT_LED_COUNT,     min = 1 }),
  brightness  = arg_schema.int ({ default = DEFAULT_BRIGHTNESS,    min = 0,   max = 255 }),
  step_ms     = arg_schema.int ({ default = DEFAULT_STEP_MS,       min = 10 }),
  duration_ms = arg_schema.int ({ default = DEFAULT_DURATION_MS,   min = 0 }),
  off_when_done = arg_schema.bool({ default = DEFAULT_OFF_WHEN_DONE }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- mode and colors come straight from args (arg_schema has no string/array helpers here)
local mode = "rainbow"
if type(args) == "table" and type(args.mode) == "string" then
  if args.mode == "cycle" or args.mode == "rainbow" then
    mode = args.mode
  end
end

local DEFAULT_PALETTE = {
  { r = 255, g = 0,   b = 0   },  -- red
  { r = 255, g = 128, b = 0   },  -- orange
  { r = 255, g = 255, b = 0   },  -- yellow
  { r = 0,   g = 255, b = 0   },  -- green
  { r = 0,   g = 0,   b = 255 },  -- blue
  { r = 200, g = 0,   b = 255 },  -- purple
}

local function clamp_byte(v)
  v = math.floor(tonumber(v) or 0)
  if v < 0 then return 0 end
  if v > 255 then return 255 end
  return v
end

local function build_palette(raw)
  if type(raw) ~= "table" or #raw == 0 then return DEFAULT_PALETTE end
  local out = {}
  for _, c in ipairs(raw) do
    if type(c) == "table" then
      out[#out + 1] = { r = clamp_byte(c.r), g = clamp_byte(c.g), b = clamp_byte(c.b) }
    end
  end
  if #out == 0 then return DEFAULT_PALETTE end
  return out
end

local palette = build_palette(args and args.colors)

print(string.format("[led_seq][DBG] mode=%s io=%d step_ms=%d duration_ms=%d brightness=%d palette_size=%d off_when_done=%s",
  mode, ctx.io, ctx.step_ms, ctx.duration_ms, ctx.brightness, #palette, tostring(ctx.off_when_done)))

local strip = nil

local function cleanup()
  if strip then
    if ctx.off_when_done then
      pcall(function() strip:clear(); strip:refresh() end)
    end
    pcall(strip.close, strip)
    strip = nil
  end
end

local function scale(channel)
  return math.floor(channel * ctx.brightness / 255)
end

local function fill_rgb(r, g, b)
  for i = 0, ctx.led_count - 1 do
    strip:set_pixel(i, r, g, b)
  end
  strip:refresh()
end

local function fill_hsv(hue)
  for i = 0, ctx.led_count - 1 do
    strip:set_pixel_hsv(i, hue, 255, ctx.brightness)
  end
  strip:refresh()
end

local function run()
  local new_strip, err = led_strip.new(ctx.io, ctx.led_count)
  if not new_strip then
    error("led_strip.new failed: " .. tostring(err))
  end
  strip = new_strip

  -- Effective stop conditions:
  --   * If duration_ms > 0, stop after that many ms have elapsed (best-effort: each step is step_ms).
  --   * If duration_ms == 0, loop forever and rely on lua_stop_async_job for cancellation.
  -- delay.delay_ms() is the cooperative cancel point: the runtime injects the
  -- stop signal there, so the script exits cleanly between iterations.
  local elapsed = 0
  local step_index = 0
  local palette_size = #palette

  while true do
    if mode == "cycle" then
      local c = palette[(step_index % palette_size) + 1]
      fill_rgb(scale(c.r), scale(c.g), scale(c.b))
    else
      fill_hsv((step_index * HSV_HUE_STEP) % 360)
    end

    delay.delay_ms(ctx.step_ms)

    step_index = step_index + 1
    if ctx.duration_ms > 0 then
      elapsed = elapsed + ctx.step_ms
      if elapsed >= ctx.duration_ms then break end
    end
  end

  print(string.format("[led_seq] done after %d steps", step_index))
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[led_seq] ERROR: " .. tostring(err))
  error(err)
end
