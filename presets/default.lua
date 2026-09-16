-- default: the current line with a soft glow and the next line dimmed below it,
-- on a subtle tinted panel. Sizes and colors come from config.lua.
--
-- draggable gives up click-through, but f:input_region keeps only the panel
-- interactive: the rest of the surface still passes clicks to the window below.

return {
  draggable = true,

  on_frame = function(f, ctx)
    local pad_x, pad_y, gap, bottom = 56, 24, 10, 64

    local current = ctx.line.text
    local next_line = ctx.next.text
    local current_size = current ~= "" and f:measure(current) or nil
    local next_size = next_line ~= "" and f:measure(next_line) or nil
    if not current_size and not next_size then return end

    -- Size the panel to what is actually drawn, so the drag handle is the
    -- visible content and nothing more.
    local width = math.max(current_size and current_size.width or 0,
                           next_size and next_size.width or 0)
    local height = 0
    if current_size then height = height + current_size.height end
    if next_size then height = height + (current_size and gap or 0) + next_size.height end

    local panel_w = width + pad_x * 2
    local panel_h = height + pad_y * 2
    local panel_x = (f.width - panel_w) * 0.5
    local panel_y = f.height - bottom - panel_h

    f:input_region(panel_x, panel_y, panel_w, panel_h)

    f:layer({ post = "bloom", uniforms = { amount = 0.5, threshold = 0.4 } }, function(l)
      l:rect({ x = panel_x, y = panel_y, w = panel_w, h = panel_h,
               color = { 0.18, 0.20, 0.38, 0.32 } })

      local y = panel_y + pad_y
      if current_size then
        l:text(current, { anchor = "top-left",
                          offset_x = (f.width - current_size.width) * 0.5,
                          offset_y = y, color = ctx.colors.current })
        y = y + current_size.height + gap
      end
      if next_size then
        l:text(next_line, { anchor = "top-left",
                            offset_x = (f.width - next_size.width) * 0.5,
                            offset_y = y, color = ctx.colors.next })
      end
    end)
  end,
}
