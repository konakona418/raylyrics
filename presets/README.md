# presets

Lua 歌词样式。放到 `~/.config/raylyrics/presets/`，在 `config.lua` 设 `preset = "名字"`，
或运行时 `raylyrics ctl preset 名字` 切换。

一个 preset 返回 `{ on_frame(f, ctx) }`：`f` 画图元/变换/后处理，`ctx` 给当前行、进度、颜色等。
`common.lua` 是共享工具模块（`require("common")`），不是 preset。

| 文件 | 说明 |
| --- | --- |
| `glow.lua` | 当前/下一行 + bloom 辉光 |
| `boxed.lua` | 底部半透明背景条 |
| `slide.lua` | 新行上滑淡入 |
| `wave.lua` | 逐字正弦波动 |
| `layeronly.lua` | 最小 layer 示例 |
| `bloomdebug.lua` | bloom/后处理调试 |

API 细节见 `../docs/PLAN.md` §7.7。
