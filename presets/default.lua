-- default: the current line with a soft glow and the next line dimmed below it.
-- Sizes and colors come from config.lua.

return {
  on_frame = function(f, ctx)
    f:layer({ post = "bloom", uniforms = { amount = 0.5, threshold = 0.4 } }, function(l)
      if ctx.line.text ~= "" then
        l:text(ctx.line.text,
               { anchor = "bottom-center", offset_y = -48, color = ctx.colors.current })
      end
      if ctx.next.text ~= "" then
        l:text(ctx.next.text,
               { anchor = "bottom-center", offset_y = -8, color = ctx.colors.next })
      end
    end)
  end,
}
