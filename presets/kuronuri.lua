-- kuronuri: a letter being censored, after Frog96's 黒塗り世界宛て書簡.
--
-- Three lines of the lyrics sit on a white sheet at a time, and the song walks
-- the sheet through the lyrics in blocks of three: a block only gives way once
-- every line in it has had its turn. Once a line is about half sung a censor's
-- bar sweeps it out from the left, and while that bar crosses the sheet shivers
-- sideways along its scanlines, on top of a steady chroma split.
--
-- The crossing is sized from the line - a longer line takes longer to cover -
-- and clamped, and it eases out exponentially rather than sliding at a constant
-- rate.
--
-- A timestamped blank line is an interlude, not a lyric, so it does not take up
-- one of the three slots.
--
-- draggable: the sheet is the drag handle, the rest of the surface stays
-- click-through.

local PAD_X, PAD_Y = 34, 26
local MIN_INNER_W = 380  -- keep the sheet looking like a sheet for short lines
local ROW_GAP = 14
local EDGE = 16          -- smallest gap between the sheet and the surface edge

local SWEEP_PER_PX = 0.0009  -- seconds of sweep per pixel of line width
local SWEEP_MIN = 0.35
local SWEEP_MAX = 1.2        -- ceiling, however long the line is
local SWEEP_EXP = 5.0        -- exponential ease-out rate
local HOLD = 0.25            -- keep a spent block up so its last sweep is seen

-- How long the line is left alone before its bar comes: half sung, clamped so
-- neither a very short nor a very long line waits an odd amount of time.
local SWEEP_AT = 0.5
local DELAY_MIN = 0.10
local DELAY_MAX = 1.5

local CHROMA = 0.50          -- channel split, held steady throughout
local SHEET_ALPHA = 0.9      -- the paper is very slightly see-through

-- Post chain, in order. bloom and the built-in scanline both read `amount`, so
-- the scanline is re-registered under its own name to get its own knob.
local POST = { "bloom", "scanline", "corrupt" }
local POST_AMOUNT = 0.45
local BLOOM_THRESHOLD = 0.70
local SCANLINE = 0.12

-- The absolute time line `i` stops being the one being sung.
local function line_end(lines, i)
  local following = lines[i + 2]
  if following then return following.time end
  local this = lines[i + 1]
  if this then return this.time + 3.0 end
  return math.huge
end

-- How far the bar has crossed by `pos`, eased out so it whips across early and
-- settles onto the line rather than sliding at a constant rate.
local function sweep(pos, start, duration)
  local x = (pos - start) / duration
  if x <= 0.0 then return 0.0 end
  if x >= 1.0 then return 1.0 end
  return (1.0 - 2.0 ^ (-SWEEP_EXP * x)) / (1.0 - 2.0 ^ -SWEEP_EXP)
end

local CORRUPT = [[
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 u_resolution;
uniform float u_time;
uniform float u_chroma;
uniform float u_noise;
out vec4 finalColor;

void main() {
  float c = clamp(u_chroma, 0.0, 1.0);
  float a = clamp(u_noise, 0.0, 1.0);
  vec2 uv = fragTexCoord;

  // The noise is in the sampling, not the colour: each scanline reads a little
  // to one side of where it should, by an amount that runs down the sheet as a
  // wave - some bands left, some right. Two sines so the ripple is not perfectly
  // regular, and a small amplitude so the text stays readable.
  float y = uv.y * u_resolution.y;
  float wave = sin(y * 0.05 + u_time * 4.0) + 0.4 * sin(y * 0.13 - u_time * 6.0);
  uv.x += wave * 0.0020 * a;

  vec4 base = texture(texture0, uv);

  // Chroma split, steady, a touch wider where the wave is steepest.
  float split = (0.004 + abs(wave) * 0.001) * c;
  float r = texture(texture0, uv + vec2(split, 0.0)).r;
  float b = texture(texture0, uv - vec2(split, 0.0)).b;

  finalColor = vec4(r, base.g, b, base.a);
}
]]

-- The built-in scanline reads the same `amount` as bloom, which would tie the
-- two together. Same effect, own uniform.
local SCANLINE_SHADER = [[
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 u_resolution;
uniform float u_time;
uniform float u_lines;
out vec4 finalColor;

void main() {
  vec4 base = texture(texture0, fragTexCoord);
  float scan = 1.0 - u_lines * (0.5 + 0.5 * sin(fragTexCoord.y * u_resolution.y * 1.5 + u_time * 6.0));
  finalColor = vec4(base.rgb * scan, base.a);
}
]]

return {
  viewport = { width = 1120, height = 320 },
  draggable = true,
  font = { size = 34 },

  on_frame = function(f, ctx)
    if #ctx.lines == 0 then return end

    local state = ctx.state
    if state.corrupt == nil then
      state.corrupt = f:shader("corrupt", CORRUPT)
      state.scanline = f:shader("scanline", SCANLINE_SHADER)
    end

    -- Drop interludes: a timestamped blank line is a rest, not a lyric.
    local lines = {}
    for i = 1, #ctx.lines do
      local entry = ctx.lines[i]
      if entry.text:match("%S") then lines[#lines + 1] = entry end
    end
    local count = #lines
    if count == 0 then return end

    local pos = ctx.position_us / 1e6

    -- The line being sung right now.
    local active = 0
    for i = 1, count do
      if lines[i].time <= pos then active = i - 1 else break end
    end

    -- Walk in blocks of three, but let the last sweep of a block finish before
    -- handing over.
    local natural = math.floor(active / 3) * 3
    local block = state.block or natural
    if active < block then
      block = natural
    elseif natural > block then
      if pos >= line_end(lines, block + 2) + HOLD then block = natural end
    end
    state.block = block

    local row_h = f:measure("Ag").line_height

    local rows, redact = {}, {}
    local text_w = 0
    for k = 0, 2 do
      local index = block + k
      local entry = lines[index + 1]
      local text = entry and entry.text or ""
      local width = f:measure(text).width
      rows[k + 1] = { text = text, width = width }
      if width > text_w then text_w = width end

      -- The bar waits for the line to be about half sung, then takes its time
      -- from how long the line is: a longer line takes longer to cover.
      local start_time = entry and entry.time or pos
      local finish = line_end(lines, index)
      local span = math.max(finish - start_time, 0.0)
      local delay = math.min(math.max(span * SWEEP_AT, DELAY_MIN), DELAY_MAX)
      local begin = start_time + delay
      local duration = math.min(math.max(width * SWEEP_PER_PX, SWEEP_MIN), SWEEP_MAX)
      -- Never let the bar run past the line's own slot.
      duration = math.min(duration, math.max(finish - begin, 0.15))
      redact[k + 1] = sweep(pos, begin, duration)
    end

    -- Clamp the sheet to the surface; a line wider than the room is scaled to
    -- fit rather than spilling past the paper.
    local avail_w = f.width - 2 * EDGE - PAD_X * 2
    local inner_w = math.min(math.max(text_w, MIN_INNER_W), avail_w)
    local fit = (text_w > inner_w and text_w > 0) and (inner_w / text_w) or 1.0

    local panel_w = inner_w + PAD_X * 2
    local panel_h = math.min(row_h * 3 + ROW_GAP * 2 + PAD_Y * 2, f.height - 2 * EDGE)
    local panel_x = (f.width - panel_w) * 0.5
    local panel_y = (f.height - panel_h) * 0.5

    f:input_region(panel_x, panel_y, panel_w, panel_h)

    -- The static rides whichever bar is crossing, so it arrives and leaves with
    -- the sweep; the chroma split just stays put.
    local noise = 0.0
    for k = 1, 3 do
      local pulse = math.sin(math.pi * redact[k])
      if pulse > noise then noise = pulse end
    end

    f:layer({ post = POST,
              uniforms = { amount = POST_AMOUNT, threshold = BLOOM_THRESHOLD,
                           lines = SCANLINE, chroma = CHROMA, noise = noise } }, function(l)
      l:rect({ x = panel_x, y = panel_y, w = panel_w, h = panel_h,
               color = { 1.0, 1.0, 1.0, SHEET_ALPHA } })

      for k = 0, 2 do
        local row = rows[k + 1]
        local y = panel_y + PAD_Y + k * (row_h + ROW_GAP)

        if row.text ~= "" then
          l:text(row.text, { anchor = "top-left", offset_x = panel_x + PAD_X,
                             offset_y = y, color = { 0.0, 0.0, 0.0, 1.0 } },
                 function(_, glyph)
                   -- Scale about the left edge so the line keeps its baseline.
                   return { offset_x = glyph.x * (fit - 1.0), offset_y = 0.0, scale = fit }
                 end)
        end

        -- The censor's bar sweeps the line from the left.
        local swept = redact[k + 1]
        if swept > 0 and row.width > 0 then
          l:rect({ x = panel_x + PAD_X - 4, y = y - 3,
                   w = (row.width * fit + 8) * swept, h = row_h + 6,
                   color = { 0.0, 0.0, 0.0, 1.0 } })
        end
      end
    end)
  end,
}
