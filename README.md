# raylyrics

Wayland 桌面歌词 overlay。通过 MPRIS 跟随当前播放的曲目，显示本地或在线（LRCLIB）同步歌词，
外观由 Lua preset 驱动（图元、变换、bloom 等后处理、自定义 shader）。

## 依赖

系统：`wayland-client`、`wayland-egl`、`egl`、`gl`、`fontconfig`、`gio-2.0`、`libsoup-3.0`、
`libcjson`、`lua5.4`、`iconv`、`wayland-scanner`；构建用 CMake + Ninja。
内置（`vendors/`）：raylib（fork）、slug-raylib、sol2。

## 构建与运行

```sh
cmake -B build -G Ninja
cmake --build build

./build/raylyrics              # 启动 overlay
./build/raylyrics --mpris      # 打印当前 MPRIS 状态（调试）
./build/raylyrics --lrc FILE   # 解析并打印一个 LRC（调试）
```

控制运行中的实例：

```sh
./build/raylyrics ctl hide | show | toggle
./build/raylyrics ctl preset <name>
./build/raylyrics ctl offset <seconds>
./build/raylyrics ctl reload | quit
```

Arch Linux 本地打包（PKGBUILD + 脚本）见 `packaging/README.md`。

## 配置

- `~/.config/raylyrics/config.lua` — 见 `config.example.lua`；可选 `on_select` / `on_metadata` /
  `on_search` 钩子来自定义选播放器与歌词匹配。
- `~/.config/raylyrics/presets/<name>.lua` — 歌词样式，示例见 `presets/`。preset 也能声明
  自己的 viewport（含全屏）、layer-shell 几何、fps、字体与颜色，覆盖 config。
- 在线歌词缓存在 `$XDG_CACHE_HOME/raylyrics/lyrics`，用 `raylyrics cache list|prune|clear|remove|dir` 管理。

详细设计见 `docs/PLAN.md`。
