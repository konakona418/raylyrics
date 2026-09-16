# presets

Lua 歌词样式。放到 `~/.config/raylyrics/presets/`，在 `config.lua` 设 `preset = "名字"`，
或运行时 `raylyrics ctl preset 名字` 切换。

一个 preset 返回 `{ on_frame(f, ctx) }`：`f` 画图元/变换/后处理，`ctx` 给当前行、进度、颜色等。
`common.lua` 是共享工具模块（`require("common")`），不是 preset。

| 文件 | 说明 |
| --- | --- |
| `default.lua` | 默认样式：当前行 + 下一行 + bloom（`preset = "default"` 会用它） |
| `glow.lua` | 当前/下一行 + bloom 辉光 |
| `boxed.lua` | 底部半透明背景条 |
| `slide.lua` | 新行上滑淡入 |
| `wave.lua` | 逐字正弦波动 |
| `prism.lua` | 全屏彩虹：字符按网格铺满全屏、粒子、旋转多边形描边、bloom |
| `layeronly.lua` | 最小 layer 示例 |

## preset 声明

preset 除 `on_frame` 外可返回一份声明来覆盖 config（只写你想改的字段）：

```lua
return {
  viewport = "fullscreen",           -- 或 { width = 1600, height = 400 }；0/负数 = 显示器尺寸
  layer = "overlay",                 -- overlay | top | bottom | background
  anchor = "bottom",                 -- 含 "full" 则四边；否则按 top/bottom/left/right 子串
  margin = { top = 0, right = 0, bottom = 0, left = 0 },
  output = "DP-1",
  namespace = "raylyrics",
  exclusive_zone = -1,               -- -1 = 不占空间
  keyboard = false,
  fps = 60,
  font = { families = { "Noto Sans CJK SC" }, size = 72, line_spacing = 16, letter_spacing = 0 },
  colors = { current = { 1, 1, 1, 1 }, next = { 1, 1, 1, 0.3 } },
  on_frame = function(f, ctx) ... end,
}
```

`viewport`/`layer`/`anchor`/`margin`/`output`/`namespace`/`exclusive_zone`/`keyboard` 在 `InitWindow()`
**之前**读取，改动需重启；`fps`/`font`/`colors` 热重载即时生效。
preset 顶层代码不要调 `f:*`（加载时还没有 GL）。

API 细节见 `../README.md` 的 "Presets" 一节。
