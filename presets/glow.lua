return {
  on_frame = function(f, ctx)
    f:layer({ post = "bloom", uniforms = { amount = 0.45, threshold = 0.5 } }, function(l)
      if ctx.line.text ~= "" then
        l:text(ctx.line.text, { anchor = "bottom-center", offset_y = -48, color = { 1, 1, 1, 1 } })
      end
      if ctx.next.text ~= "" then
        l:text(ctx.next.text, { anchor = "bottom-center", offset_y = -8, color = { 1, 1, 1, 0.3 } })
      end
    end)
  end,
}
