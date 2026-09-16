-- kuronuri: a letter being censored, after Frog96's 黒塗り世界宛て書簡.
--
-- Three lines of the lyrics sit on a white sheet at a time, and the song walks
-- the sheet through the lyrics in blocks of three. Once a line is about half
-- sung a censor's bar sweeps it out from the left, and the block hands over the
-- moment the last of its three bars lands - not when the song reaches the next
-- line - so the sheet never sits fully censored waiting for the lyrics.
--
-- The handover is a roll: the spent block slides up and out while the next one
-- comes up from below, eased at both ends so it settles rather than stopping.
-- The sheet itself is a fixed size, so only the words move.
--
-- While a bar crosses the sheet shivers sideways along its scanlines, on top of
-- a steady chroma split, bloom and scanlines.
--
-- A timestamped blank line is an interlude, not a lyric, so it does not take up
-- one of the three slots.
--
-- draggable: the sheet is the drag handle, the rest of the surface stays
-- click-through.

local PAD_X, PAD_Y = 34, 30
local ROW_GAP = 14
local BAR_PAD = 3        -- a bar overhangs its line a little at both ends

local SLIDE_TIME = 0.30  -- how long the roll takes
local SLIDE_EXP = 5.0    -- exponential ease-in-out rate for the roll

local SWEEP_PER_PX = 0.0009  -- seconds of sweep per pixel of line width
local SWEEP_MIN = 0.35
local SWEEP_MAX = 1.2        -- ceiling, however long the line is
local SWEEP_EXP = 5.0        -- exponential ease-out rate

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

-- Eased at both ends, exponentially: the roll creeps away, gathers, and settles
-- onto the next block rather than starting and stopping dead.
local function ease_in_out(x)
  if x <= 0.0 then return 0.0 end
  if x >= 1.0 then return 1.0 end
  local span = 2.0 ^ SLIDE_EXP - 1.0
  if x < 0.5 then return 0.5 * (2.0 ^ (2.0 * SLIDE_EXP * x) - 1.0) / span end
  return 1.0 - 0.5 * (2.0 ^ (2.0 * SLIDE_EXP * (1.0 - x)) - 1.0) / span
end

-- Everything about line `i`'s bar: how wide the line is, when the bar arrives
-- and how long it takes. The wait is half the line and the crossing is sized
-- from the line, both clamped.
local function bar_timing(f, lines, i)
  local entry = lines[i + 1]
  if entry == nil then return nil end

  local width = f:measure(entry.text).width
  local finish = line_end(lines, i)
  local span = math.max(finish - entry.time, 0.0)

  local delay = math.min(math.max(span * SWEEP_AT, DELAY_MIN), DELAY_MAX)
  local begin = entry.time + delay
  local duration = math.min(math.max(width * SWEEP_PER_PX, SWEEP_MIN), SWEEP_MAX)
  -- Never let the bar run past the line's own slot.
  duration = math.min(duration, math.max(finish - begin, 0.15))

  return { width = width, begin = begin, duration = duration, finish = begin + duration }
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

    -- The block walks on its own: the moment the last of its three bars lands it
    -- hands over, whether or not the song has reached the next line yet. Only a
    -- seek - or the lyrics catching up - pulls it back into line.
    local natural = math.floor(active / 3) * 3
    local block = state.block or natural
    if natural > block or natural < block - 3 then block = natural end
    while lines[block + 4] ~= nil do
      local last = bar_timing(f, lines, block + 2)
      if last == nil or pos < last.finish then break end
      block = block + 3
    end

    -- Handing over: remember what is leaving, and start the roll.
    if state.block ~= block or state.slide_at == nil then
      state.prev = state.widths
      state.slide_at = pos
      state.block = block
    end

    local row_h = f:measure("Ag").line_height
    local row_step = row_h + ROW_GAP
    local block_h = row_step * 3 - ROW_GAP

    local rows, redact, widths = {}, {}, {}
    local text_w = 0
    for k = 0, 2 do
      local index = block + k
      local entry = lines[index + 1]
      local timing = bar_timing(f, lines, index)
      local text = entry and entry.text or ""
      local width = timing and timing.width or 0
      rows[k + 1] = { text = text, width = width }
      widths[k + 1] = width
      if width > text_w then text_w = width end
      redact[k + 1] = timing and sweep(pos, timing.begin, timing.duration) or 0.0
    end
    state.widths = widths

    -- The sheet fills the surface, so a block can roll clean off the top edge
    -- and the next can come in from below it.
    local panel_x, panel_y = 0, 0
    local panel_w = f.width
    local panel_h = f.height
    local row_top = panel_y + (panel_h - block_h) * 0.5

    -- Exactly far enough that the outgoing block has cleared the top edge - bars
    -- and all - as the incoming one arrives from below. The row band is centred,
    -- so this is the distance from its bottom to the top of the sheet.
    local travel = (panel_h + block_h) * 0.5 + BAR_PAD
    local eased = ease_in_out((pos - state.slide_at) / SLIDE_TIME)
    local lift = travel * eased            -- how far the outgoing block has gone
    local rise = travel * (1.0 - eased)    -- where the incoming block starts

    -- A line wider than the room is scaled to fit rather than spilling out.
    local inner_w = panel_w - PAD_X * 2
    local fit = (text_w > inner_w and text_w > 0) and (inner_w / text_w) or 1.0

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

      -- The block on its way out is already three solid bars. Drawn for the
      -- whole roll, first frame included, so the handover never blinks: on that
      -- frame lift is still 0 and these are simply the bars already on screen.
      if state.prev and eased < 1.0 then
        for k = 0, 2 do
          local width = state.prev[k + 1] or 0
          if width > 0 then
            local y = row_top - lift + k * row_step
            l:rect({ x = panel_x + PAD_X - 4, y = y - BAR_PAD,
                     w = width * fit + 8, h = row_h + BAR_PAD * 2, color = { 0.0, 0.0, 0.0, 1.0 } })
          end
        end
      end

      for k = 0, 2 do
        local row = rows[k + 1]
        local y = row_top + rise + k * row_step

        if row.text ~= "" then
          local options = { anchor = "top-left", offset_x = panel_x + PAD_X,
                            offset_y = y, color = { 0.0, 0.0, 0.0, 1.0 } }
          if fit < 1.0 then
            -- Scale about the left edge so the line keeps its baseline.
            l:text(row.text, options, function(_, glyph)
              return { offset_x = glyph.x * (fit - 1.0), offset_y = 0.0, scale = fit }
            end)
          else
            l:text(row.text, options)
          end
        end

        -- The censor's bar sweeps the line from the left.
        local swept = redact[k + 1]
        if swept > 0 and row.width > 0 then
          l:rect({ x = panel_x + PAD_X - 4, y = y - BAR_PAD,
                   w = (row.width * fit + 8) * swept, h = row_h + BAR_PAD * 2,
                   color = { 0.0, 0.0, 0.0, 1.0 } })
        end
      end
    end)
  end,
}
