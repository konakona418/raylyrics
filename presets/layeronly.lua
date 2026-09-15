return {
  on_frame = function(f, ctx)
    f:layer({}, function(l)
      if ctx.line.text ~= "" then
        l:text(ctx.line.text, { anchor = "bottom-center", offset_y = -48, color = {1, 1, 1, 1} })
      end
      if ctx.next.text ~= "" then
        l:text(ctx.next.text, { anchor = "bottom-center", offset_y = -8, color = {1, 1, 1, 0.35} })
      end
    end)
  end,
}
