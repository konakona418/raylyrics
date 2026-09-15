return {
  on_frame = function(f, ctx)
    if ctx.line.text == "" then return end
    f:text(ctx.line.text, { anchor = "bottom-center", offset_y = -48, color = {1, 1, 1, 1} },
      function(i, g)
        local wave = math.sin(ctx.wall_time * 4.0 + i * 0.6) * 6.0
        local appear = math.max(0.0, math.min(1.0, ctx.line.age * 4.0 - i * 0.12))
        return { offset_y = wave, alpha = appear }
      end)
    if ctx.next.text ~= "" then
      f:text(ctx.next.text, { anchor = "bottom-center", offset_y = -8, color = {1, 1, 1, 0.3} })
    end
  end,
}
