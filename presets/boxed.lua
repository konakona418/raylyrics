return {
  on_frame = function(f, ctx)
    f:rect({ x = 0, y = f.height - 90, w = f.width, h = 90, color = {0, 0, 0, 0.5} })
    if ctx.line.text ~= "" then
      f:text(ctx.line.text, { anchor = "bottom-center", offset_y = -48, color = {1, 1, 1, 1} })
    end
    if ctx.next.text ~= "" then
      f:text(ctx.next.text, { anchor = "bottom-center", offset_y = -8, color = {1, 1, 1, 0.35} })
    end
  end,
}
