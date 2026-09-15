return {
  on_frame = function(f, ctx)
    f:layer({ post = "bloom", uniforms = { amount = 0.45, threshold = 0.5 } }, function(l)
      l:rect({ x = 0, y = 0, w = f.width, h = f.height, color = { 0.1, 0.1, 0.2, 0.6 } })
      local line = ctx.line.text
      if line == "" then line = "BLOOM TEST 你好" end
      l:text(line, { anchor = "center", offset_y = -50, color = { 1, 1, 1, 1 } })
      if ctx.next.text ~= "" then
        l:text(ctx.next.text, { anchor = "center", offset_y = 60, color = { 1, 1, 1, 0.5 } })
      end
    end)
  end,
}
