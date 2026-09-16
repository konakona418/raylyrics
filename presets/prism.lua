-- prism: fullscreen rainbow. Only the current line, characters spread across
-- the screen along a slow sine path (reading order preserved), each with its
-- own drifting transform, plus drifting particles and rotating polygon
-- outlines, all fed through bloom.
--
-- The preset declares its own fullscreen surface, so no config changes needed.

local TAU = math.pi * 2.0

local function hsv(h, s, v, a)
  h = h - math.floor(h)
  local i = math.floor(h * 6.0)
  local f = h * 6.0 - i
  local p = v * (1.0 - s)
  local q = v * (1.0 - f * s)
  local t = v * (1.0 - (1.0 - f) * s)
  i = i % 6
  local r, g, b
  if i == 0 then r, g, b = v, t, p
  elseif i == 1 then r, g, b = q, v, p
  elseif i == 2 then r, g, b = p, v, t
  elseif i == 3 then r, g, b = p, q, v
  elseif i == 4 then r, g, b = t, p, v
  else r, g, b = v, p, q end
  return { r, g, b, a or 1.0 }
end

local function noise(a, b)
  local x = math.sin(a * 127.1 + b * 311.7) * 43758.5453
  return x - math.floor(x)
end

local function draw_polygons(l, f, time)
  local sides = { 3, 5, 6 }
  for s = 1, #sides do
    local n = sides[s]
    local cx = f.width * (0.5 + 0.34 * math.sin(time * 0.07 + s * 2.1))
    local cy = f.height * (0.5 + 0.34 * math.cos(time * 0.05 + s * 1.7))
    local radius = math.min(f.width, f.height) * (0.16 + 0.05 * s)
    local spin = time * (0.05 + 0.02 * s) * (s % 2 == 0 and 1.0 or -1.0)
    local prev_x, prev_y
    for k = 0, n do
      local a = spin + k * TAU / n
      local px = cx + math.cos(a) * radius
      local py = cy + math.sin(a) * radius
      if prev_x then
        l:line({ x1 = prev_x, y1 = prev_y, x2 = px, y2 = py, thickness = 2.0,
                 color = hsv(time * 0.03 + s * 0.18, 0.8, 1.0, 0.16) })
      end
      prev_x, prev_y = px, py
    end
  end
end

local function draw_particles(l, f, ctx, time)
  local particles = ctx.state.particles
  if not particles then
    particles = {}
    ctx.state.particles = particles
  end

  local dt = math.min(ctx.dt, 0.05)
  local budget = 180 - #particles
  local spawn = budget > 0 and math.min(4, budget) or 0
  for _ = 1, spawn do
    particles[#particles + 1] = {
      x = math.random() * f.width,
      y = f.height * (0.55 + math.random() * 0.45),
      vx = (math.random() - 0.5) * 26.0,
      vy = -(6.0 + math.random() * 26.0),
      life = 0.0,
      max_life = 3.5 + math.random() * 5.0,
      radius = 1.5 + math.random() * 3.5,
      hue = math.random(),
    }
  end

  for i = #particles, 1, -1 do
    local p = particles[i]
    p.life = p.life + dt
    if p.life >= p.max_life then
      table.remove(particles, i)
    else
      p.x = p.x + p.vx * dt
      p.y = p.y + p.vy * dt
      local fade = 1.0 - p.life / p.max_life
      l:circle({ x = p.x, y = p.y, radius = p.radius,
                 color = hsv(p.hue + time * 0.02, 0.7, 1.0, fade * 0.45) })
    end
  end
end

-- Count UTF-8 codepoints; one glyph per codepoint (no shaping).
local function utf8_count(text)
  local count = 0
  for i = 1, #text do
    local b = text:byte(i)
    if b < 0x80 or b >= 0xC0 then count = count + 1 end
  end
  return count
end

local FONT_SIZE = 200  -- declared below; Slug is resolution independent, so the
                       -- per-glyph scale keeps it relative to the screen.

local function draw_lyric(l, f, ctx, time)
  local text = ctx.line.text
  if text == "" then return end

  -- Lay the glyphs out on a grid whose column count fits the screen aspect.
  -- Row-major order keeps reading order.
  local n = math.max(utf8_count(text), 1)
  local aspect = f.width / math.max(f.height, 1.0)
  local cols = math.ceil(math.sqrt(n * aspect))
  cols = math.max(1, math.min(cols, n))
  local rows = math.ceil(n / cols)

  local scale_base = math.min(f.width, f.height) / 1440.0
  local seed_line = (ctx.line.index + 1) * 0.618
  local appear = math.min(1.0, ctx.line.age / 0.5)

  l:text(text, { anchor = "top-left" }, function(i, g)
    local col = i % cols
    local row = math.floor(i / cols)
    local row_count = math.min(cols, n - row * cols)
    local t = (col + 0.5) / row_count
    local u = (row + 0.5) / rows
    local seed = noise(i + 1, seed_line)

    -- Grid position keeps reading order; the sine gives each row vertical life.
    local base_x = f.width * (0.5 + (t - 0.5) * 0.86)
    local base_y = f.height * (0.5 + (u - 0.5) * 0.74)
        + math.sin(time * 0.18 + row * 1.3 + seed_line) * f.height * 0.025

    local px = base_x + math.sin(time * 0.33 + seed * TAU) * f.width * 0.014
    local py = base_y + math.cos(time * 0.27 + seed * TAU) * f.height * 0.028
    local rotation = math.sin(time * 0.30 + seed * TAU) * 16.0
    local scale = scale_base * (1.0 + math.sin(time * 0.21 + seed * TAU) * 0.22)
    local hue = t * 0.8 + u * 0.2 + time * 0.04 + seed * 0.15

    return {
      offset_x = px - g.x,
      offset_y = py - g.y,
      scale = scale,
      rotation = rotation,
      alpha = appear,
      color = hsv(hue, 0.85, 1.0),
    }
  end)
end

return {
  viewport = "fullscreen",
  anchor = "fullscreen",
  margin = { top = 0, right = 0, bottom = 0, left = 0 },
  layer = "overlay",
  font = { size = FONT_SIZE },

  on_frame = function(f, ctx)
    local time = ctx.wall_time

    -- Dim the desktop behind everything; keep it out of the bloom pass.
    f:rect({ x = 0, y = 0, w = f.width, h = f.height, color = { 0.02, 0.01, 0.05, 0.45 } })

    f:layer({ post = "bloom", uniforms = { amount = 0.8, threshold = 0.30 } }, function(l)
      draw_polygons(l, f, time)
      draw_particles(l, f, ctx, time)
      draw_lyric(l, f, ctx, time)
    end)
  end,
}
