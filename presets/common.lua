local common = {}

function common.clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

function common.lerp(a, b, t)
  return a + (b - a) * t
end

function common.ease_out_cubic(t)
  t = common.clamp(t, 0, 1)
  return 1 - (1 - t) ^ 3
end

function common.ease_in_out(t)
  t = common.clamp(t, 0, 1)
  return t * t * (3 - 2 * t)
end

return common
