# presets

Lua lyric styles. Copy a file to `~/.config/raylyrics/presets/`, then select it
with `preset = "name"` in `config.lua` or `raylyrics ctl preset name` at runtime.

A preset returns `{ on_frame(f, ctx) }`: `f` draws primitives, transforms and
post-processing; `ctx` carries the current line, progress, colors and a
persistent `state` table. `common.lua` is a shared module loaded with
`require("common")`, not a preset.

| File | Description |
| --- | --- |
| `default.lua` | Default look: current line + next line + bloom (used by `preset = "default"`) |
| `glow.lua` | Current and next line with a bloom glow |
| `boxed.lua` | Translucent bar behind the text |
| `slide.lua` | New line slides up and fades in |
| `wave.lua` | Per-character sine wave |
| `prism.lua` | Fullscreen rainbow: characters on a screen-filling grid, particles, rotating polygon outlines, bloom |
| `layeronly.lua` | Minimal `layer` example |

## Declarations

Besides its hooks, a preset may return settings that override `config.lua`. Only
the fields you write are applied.

```lua
return {
  viewport = "fullscreen",           -- or { width = 1600, height = 400 }; 0/negative = output size
  layer = "overlay",                 -- overlay | top | bottom | background
  anchor = "bottom",                 -- contains "full" for all edges, else top/bottom/left/right
  margin = { top = 0, right = 0, bottom = 0, left = 0 },
  output = "DP-1",
  namespace = "raylyrics",
  exclusive_zone = -1,               -- -1 = reserve no space
  keyboard = false,
  draggable = false,                 -- left-button dragging (gives up click-through)
  fps = 60,
  font = { families = { "Noto Sans CJK SC" }, size = 72, line_spacing = 16, letter_spacing = 0 },
  colors = { current = { 1, 1, 1, 1 }, next = { 1, 1, 1, 0.3 } },
  on_frame = function(f, ctx) ... end,
}
```

`viewport`, `layer`, `anchor`, `margin`, `output`, `namespace`, `exclusive_zone`,
`keyboard` and `draggable` are read **before** `InitWindow()`, so changing them
needs a restart. `fps`, `font` and `colors` are re-applied on hot reload and on
`ctl preset` / `ctl reload`. A preset's top-level code must not call `f:*` —
there is no GL context yet when the file is evaluated.

See the "Presets" section of `../README.md` for the full API.
