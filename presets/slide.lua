local common = require("common")

return {
  on_frame = function(f, ctx)
    if ctx.line.text == "" then return end
    local t = common.ease_out_cubic(ctx.line.age / 0.4)
    f:text(ctx.line.text, {
      anchor = "bottom-center",
      offset_y = -48 + (1.0 - t) * 18.0,
      color = { 1, 1, 1, t },
    })
    if ctx.next.text ~= "" then
      f:text(ctx.next.text, { anchor = "bottom-center", offset_y = -8, color = { 1, 1, 1, 0.3 } })
    end
  end,
}
