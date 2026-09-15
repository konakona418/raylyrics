# raylyrics 设计文档

轻量、原生 Wayland 的桌面歌词 overlay。C/C++，优先 Firefox + 本地 LRC。

MVP 要验证的链路：

```text
Firefox → MPRIS(GDBus) → media_state → LRC → lyric_state
        → display list → raylib(Slug 文本) → layer-shell overlay
```

## 1. 范围

MVP 内：

- Firefox 的 MPRIS 播放状态与 metadata
- 本地 `.lrc`（标准 + 增强型）解析与匹配
- 桌面底部透明 overlay，`zwlr_layer_shell_v1`
- 整行高亮（当前行亮、下一行暗）；逐字高亮已放弃（见 §7.6）
- Lua 配置与插件化绘制（受限 display list）

MVP 外（后续）：在线歌词源、多播放器、IPC/CLI、多 compositor 兼容、GPU/dmabuf 优化、复杂动画特效。

## 2. 分层与数据流

```text
src/media     MPRIS/GDBus ─┐
src/lyrics    LRC + 同步  ─┼─→ lyric_state ─┐
src/config    Lua 配置    ─┘               │
                                            ▼
src/plugin    Lua 宿主 ──→ display list ──→ src/render (Slug 文本)
                                                    │
                                                    ▼
                                          src/wayland (rcore 后端 + EGL)
                                                    │
                                                    ▼
                                          wlroots/KWin layer-shell overlay
```

模块边界：模块内部用 C++，对外以 `extern "C"` + 不透明指针暴露（见 §4）。

## 3. 技术栈决策

| 层 | 决定 | 理由 |
|---|---|---|
| 语言 | C++20（自有代码）+ C（raylib 平台文件） | 允许 C++ 后，仅 rcore 平台文件被迫保持 C |
| 构建 | CMake + Ninja（沿用现状） | 已有配置；协议生成用 `add_custom_command` |
| 渲染 | raylib **fork 进 `vendors/raylib`**（去 submodule） | 需改 `rcore.c` 加平台分支 |
| 平台后端 | 自写 `rcore_wayland_layer`：`wl_display` + `zwlr_layer_shell_v1`(绑 v4) + `wl_egl_window` + EGL | 唯一能同时满足 raylib 与真正 overlay |
| 图形 API | `GRAPHICS_API_OPENGL_43`（EGL 走 `EGL_OPENGL_API`/`EGL_OPENGL_BIT`，context 4.3 core） | Slug shader 是 `#version 430` + SSBO |
| 文本 | 外部库 `slug-raylib`（Slug GPU glyph 渲染，轮廓来自 stb_truetype，回退由 fontconfig） | 高精度、逐 glyph 控制；免去 Pango/Cairo |
| 高亮 | 整行级：当前行亮、下一行暗。**不做逐字高亮** | 行级 LRC 无字级时间，插值在长间奏处失真 |
| D-Bus | GDBus (`gio-2.0`) | 不引入 libsystemd；GLib 仅用于 GIO |
| 位置同步 | Firefox `Position` 仅作校准；`CLOCK_MONOTONIC` × `Rate` 推进 | Firefox Position 稀疏且不准 |
| 插件/配置 | Lua 5.4 (PUC) + sol2；**全部可信**；config 即 Lua | 单一语言，表达力强 |
| 事件循环 | raylib 主循环 + 每帧 `g_main_context_iteration(NULL,FALSE)`；**放弃** epoll/timerfd | raylib/GLFW 不暴露 Wayland fd |
| 节奏 | 固定 tick 16–33ms | 与上一条一致 |
| 输出 | 单输出 MVP，`buffer_scale=1`，忽略 fractional scale | 最小可行 |
| 输入 | 完全无输入；`keyboard_interactivity=none` + 空 input region | click-through / no-focus |
| 歌词 | 可配置根目录（默认 `~/.local/share/raylyrics/lyrics/`），`Artist - Title.lrc`；UTF-8 → GB18030(iconv) | Firefox 不给本地路径，必须有根目录 |
| 播放器 | Firefox `instance_*` 优先，忽略 playerctld，回退唯一 Playing | Firefox-first |
| 合成 | 内建 premultiplied alpha | Wayland wl_surface 语义要求预乘 |

明确不用：GTK/Qt、Pango、Cairo、libass、OpenGL ES、wl_shm 主路径、epoll。

## 4. 语言与错误处理

- C++20，`-fno-exceptions -fno-rtti`。
- 模块内用 C++（RAII/std 容器），模块间 `extern "C"` 不透明指针。
- `platforms/rcore_wayland_layer.c` **必须是 C**：它被 `rcore.c`（C 文件）`#include`。
- Lua 绑定用 **sol2**。
- 待验证：sol2 在 `-fno-exceptions`（`SOL_NO_EXCEPTIONS`）下是否完整可用，以及是否依赖 RTTI。若不满足，回退到手写 C 绑定层。
- 错误策略：内部用错误码/返回值（无异常），C 回调（GDBus/Wayland）不得逃逸。

## 5. 目录结构

```text
src/
  main.cpp
  media/     media.h media.cpp mpris.cpp
  lyrics/    lyrics.h lrc.cpp matcher.cpp
  render/    render.h display_list.h text.h slug.cpp
  plugin/    plugin.h lua_host.cpp
  config/    config.h config.cpp
  wayland/   wayland.h egl.c layer_shell.c        # C
protocols/   wlr-layer-shell-unstable-v1.xml
vendors/     raylib/ (fork) sol2/ slug-raylib/
docs/        PLAN.md
```

## 6. 数据模型

```c
struct media_state {
    char    *title;
    char    *artist;
    char    *album;
    bool     playing;
    int64_t  position_us;   /* 最近一次校准值 */
    double   rate;          /* PlaybackRate */
    uint64_t generation;    /* metadata/track 变化递增 */
};

struct lyric_syllable {
    int64_t t_us;
    char   *text;
};

struct lyric_line {
    int64_t           t_us;
    char             *text;
    struct lyric_syllable *syllables;  /* 增强 LRC；可空 */
    size_t            n_syllables;
};

struct lyric_doc {
    struct lyric_line *lines;   /* 按 t_us 排序 */
    size_t             n;
    int64_t            offset_us;
};
```

## 7. 关键设计

### 7.1 layer-shell 握手（Phase 1 关键路径）

`InitPlatform()` 必须在 `eglCreateWindowSurface` 前完成：

```text
wl_display_connect
  → registry: wl_compositor / wl_shm / zwlr_layer_shell_v1 / wl_output
  → wl_surface
  → zwlr_layer_surface_v1 (layer=overlay, anchor=bottom|left|right,
                           exclusive_zone=-1, keyboard_interactivity=none)
  → set_size / set_margin
  → commit
  → 阻塞 dispatch 直到 configure
  → ack_configure
  → wl_egl_window + EGL config(alpha>=8) + context
  → rlglInit
```

raylib 的 `InitWindow` 是同步的，此处的 roundtrip 自行处理。

### 7.2 透明合成

`rlSetBlendFactors(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` + `ClearBackground(0,0,0,0)`。否则半透明文字边缘会出现光晕。

### 7.3 Position 同步

```text
pos(t) = mpris_position_us + (monotonic_now - position_ts) * rate
```

重同步时机：`PlaybackStatus` 变化、`Seeked`、`mpris:trackid` 变化、metadata 更新。

### 7.4 MPRIS / 多播放器

- bus name：`org.mpris.MediaPlayer2.<app>`；忽略 `playerctld`。
- object：`/org/mpris/MediaPlayer2`，iface `org.mpris.MediaPlayer2.Player`（Identity 在 `org.mpris.MediaPlayer2`）。
- **多播放器**：backend 持有所有播放器，各自 `GetAll` + 订阅 `PropertiesChanged`/`Seeked`；
  `NameOwnerChanged`（namespace 匹配）增删。`identity` 取根接口的 `Identity`。
- 选择：`media_selection_dirty()`（播放器集合或状态/元数据变化）时，main 把播放器列表交给
  `config.on_select`；无钩子/无选择则用内置策略（正在播放的当前播放器 → 正在播放的 Firefox
  → 当前 → 任意 Firefox → 第一个）。`media_set_active()` 落地选择。
- 只有活动播放器会同步采样 `Position`（其余靠 `PropertiesChanged` 更新状态）。
- 字段：`xesam:title`、`xesam:artist`(as)、`xesam:album`、`mpris:trackid`(o)、`mpris:length`(x)、`PlaybackStatus`(s)、`Position`(x)、`Rate`(d)。
- Firefox 注意：仅在有媒体时出现；`Position` 稀疏；`xesam:url` 可能是 `file://` 或 `http(s)://`；
  `mpris:trackid` 跨曲固定，换歌靠 metadata 签名（title/artist/album/length）判断。
- `state.generation` 由 backend 维护：活动播放器切换或曲目签名变化时自增，驱动换歌重载。

### 7.5 LRC

- 标签：`[ar:] [ti:] [al:] [by:] [offset:] [length:]`。
- 时间戳：`[mm:ss]`、`[mm:ss.xx]`、`[mm:ss.xxx]`，一行可多个。
- 增强型：`<mm:ss.xx>` 逐字/逐词。
- 加载后按 `t_us` 排序；当前行用二分查找（不逐帧线性扫描）。
- 编码：UTF-8（含 BOM）优先，失败尝试 GB18030（iconv）。

### 7.6 文本与高亮

实现位于 `src/render/text.{h,cpp}`，底层是 `vendors/slug-raylib/slug.h`。

- **轮廓**：stb_truetype。CFF/OTF（如 Noto Sans CJK 的 `.ttc`）产出的是三次贝塞尔
  `STBTT_vcubic`，而 Slug shader 只解二次，因此在 `t=0.5` 处 de Casteljau 拆成两段二次
  （与端点、中点重合）后再打包。
- **字体回退**：fontconfig 按 codepoint 选字体（`FC_CHARSET` + `FC_FAMILY` 优先列表），
  按 `(file, faceIndex)` 分组，每组建一个 `SlugFont`。`.ttc` 用 `stbtt_GetFontOffsetForIndex`。
- **布局**：按 `\n` 分行；逐 codepoint 用各自字体的 advance 累加；baseline 用首个字体的
  ascent 对齐；支持整体居中或**逐行居中**；暴露 `LineWidth()`/`Width()`/`Height()`。
- **高亮**：**整行级**——当前行全亮、下一行变暗。`TextRenderer::DrawLines(x, y, first, last,
  hl_start, hl_end, base, highlight, center)` 仍支持按 cluster 区间上色（`DrawHighlighted`
  同理），但 overlay 不再使用。
- **接口**（当前）：
  ```cpp
  TextRenderer::Prepare(all_song_text, TextStyle{families, size, line_spacing, letter_spacing});
  TextRenderer::SetText(line_or_block);   // 只做布局，复用 Prepare 建好的字体
  TextRenderer::Draw(x, y, color, center);
  TextRenderer::DrawLines(x, y, first_line, last_line, hl_start, hl_end, base, highlight, center);
  TextRenderer::LineWidth(line); TextRenderer::Width(); TextRenderer::Height(); TextRenderer::LineHeight();
  ```

**为什么放弃逐字高亮**：LRCLIB/本地行级 LRC 只有行时间戳，没有字级时间。按字符数线性插值在
长间奏处会严重失真（例如 `Sumatra Keibitai` 的 `[01:41.32]` 到 `[02:03.41]` 隔了 22 秒，
整句 18 个字被拉成每字 ~1.2 秒）。`lrc_highlight_count()` 仍保留（增强 LRC 的 `<mm:ss.xx>`
可精确驱动），但当前不启用。

**已知限制**：

- 未接入 HarfBuzz shaping。CJK/拉丁可用（拉丁用 stbtt kerning）；Devanagari/Arabic 等需要
  重排/连字的文字会错位。
- **彩色 emoji 无法渲染**：Noto Color Emoji 是 CBDT/CBLC 位图字体，没有 `glyf` 轮廓，Slug
  取不到曲线。要显示彩色 emoji 需另走纹理上传路径；单色符号字体可走 Slug。


### 7.7 Lua 配置 / preset

**配置**（`~/.config/raylyrics/config.lua`，`src/config/`）：`overlay{anchor,margin_*,width,height}`、
`fps`、`font{families,size,line_spacing,letter_spacing}`、`colors{current,next}`、`lyrics{root}`、
`preset`。overlay 几何必须在 `InitWindow()` 前拿到，所以配置在 Lua 层之前加载，再经
`rl_wl_set_geometry()` 传给 Wayland 后端。

**preset**（`src/plugin/`，sol2 + Lua 5.4）：
- 文件返回一个表：`{ on_frame(f, ctx), on_lyric(ctx), on_metadata(ctx) }`。
- 命令式全帧回调；`ctx` 给 `position_us/dt/wall_time/seeked`、`line{text,index,age,progress,duration}`、
  `next{text}`、`lines[{text,index,time}]`、`index/count`、`state`（跨帧持久表）、`colors`。
- `f` 方法：
  - 图元：`text/rect/line/circle/triangle/image`
  - 变换：`push/pop/translate/rotate/scale`（矩阵栈）
  - `measure(str) -> {width,height,line_height,line_count,lines}`（`lines` 是逐行宽度数组）
  - `shader(name, frag) -> bool` 注册自定义全屏后处理 shader（编译失败返回 `false` 并记日志）
  - `uniforms({...})` 设置本帧的 ambient uniform，作用于之后每个 layer 的 shader；同名时 layer 自身的 `uniforms` 覆盖它
  - `layer({post, uniforms}, function(l) ... end)`：子内容渲染到 FBO，按 `post` 链做全屏后处理再合成
  - `width/height`：surface 尺寸
- uniform 取值形式：数字→`float`，布尔→`int`(0/1)，数组→`vec2/3/4`，
  显式 `{ type = "int"|"float"|"vec2"|"vec3"|"vec4", value = ... }` 覆盖推断；
  并自动注入 `u_resolution`/`u_time`/`u_line_progress`。
- 逐 glyph 回调：`f:text(str, opts, function(i, g) return {offset_x,offset_y,scale,rotation,alpha,color} end)`，
  C 负责排版 + Slug 渲染，Lua 只给每字覆盖。`g = {x,y,codepoint,cluster,line}`。
- anchor：`"bottom-center"` 等，按 surface 尺寸解析。
- 内置后处理：`bloom`（4 级 downsample→加性 upsample 金字塔，`amount`/`threshold`，每级 upsample 权重
  `0.7` 以免过曝，默认 `amount=0.6`）、`chroma`、`blur`、`scanline`。
- C 侧持有命令缓冲（`src/plugin/commands.h`），每帧复用；`f:*` 只追加命令，回调结束后统一执行。
- 出错时记一次日志、禁用该 preset、回退内置默认样式（`kDefaultPreset`）。
- **热重载**：每秒轮询 preset 文件 mtime，变化则清自定义 shader、重置 `state`、重新执行文件。
- **复用**：`require("common")` 可用（`package.path` 已包含 presets 目录）。
- 信任模型：全部可信，不做沙箱。
- preset 可通过全局 `config` 读取配置（`PluginHost::LoadConfig()` 在 preset Lua state 里再执行一次
  `config.lua`），例如 `config.preset_params`。

### 7.8 歌词来源

- **本地优先**：`file://` URL → 同目录同名 `.lrc`；否则 `<歌词根目录>/Artist - Title.lrc`
  （默认 `~/.local/share/raylyrics/lyrics/`）。
- **LRCLIB 回退**：`GET https://lrclib.net/api/get?track_name=&artist_name=&album_name=&duration=`，
  取 `syncedLyrics`（标准 LRC）；无同步歌词时退 `plainLyrics`（无时间轴）。libsoup 异步，回调在
  主循环的 GMainContext 上触发；cJSON 解析。`/api/get` 非 200 时自动回退 `/api/search`，
  在结果里挑 duration 最接近且有 `syncedLyrics` 的条目。
- 匹配时处理 Firefox/Spotify 怪癖：artist 为空则按 `" • "` / `" - "` 拆分 title。
- **编码**：`src/lyrics/encoding.cpp` 处理 UTF-8/UTF-16 BOM，非 UTF-8 回退 GB18030（iconv）。
- 手动指定：`raylyrics --lrc FILE` 解析并打印（调试）。

### 7.9 IPC / CLI

Unix socket（`$XDG_RUNTIME_DIR/raylyrics.sock`，`src/ipc/control.cpp`），overlay 每帧非阻塞轮询：

```
raylyrics ctl hide | show | toggle
raylyrics ctl preset <name>
raylyrics ctl offset <seconds>     # 歌词偏移，立即生效
raylyrics ctl reload
raylyrics ctl quit
```

`offset` 直接加到媒体 position 上再算当前行，所以不用改 LRC 文件。

`cache` 子命令直接操作磁盘缓存，不需要运行中的实例：

```
raylyrics cache list               # key / artist - title / album / duration / size / age / source
raylyrics cache remove <key>
raylyrics cache prune              # 删除已过期条目
raylyrics cache clear
raylyrics cache dir                # 打印缓存目录
```

### 7.10 逻辑钩子（config.lua）

选播放器与歌词匹配的策略放在 `config.lua`（只加载一次，不随视觉 preset 热重载）。
实现：`PluginHost::LoadConfig()` 在 preset 的 Lua state 里执行 `config.lua`，钩子以全局 `config` 的函数形式存在；
`main` 负责编排，media/lrclib 保持无 Lua。

```lua
-- 选活动播放器。返回 bus name 或 1-based 索引；nil 用内置策略。
config.on_select = function(players)  -- players[i] = { name, identity, title, artist, album, playing, position_us, length_us }
  for i, p in ipairs(players) do
    if p.playing and p.identity == "Spotify" then return i end
  end
end

-- 匹配前归一化 metadata（如拆分 "Artist • Title"）。
config.on_metadata = function(m)  -- { artist, title, album, duration, player }
  local artist, title = m.title:match("^(.-) • (.*)$")
  if artist then return { artist = artist, title = title } end
end

-- 从 LRCLIB /api/search 候选中挑选（1-based 索引；nil 用内置启发式）。
config.on_search = function(query, results)  -- results[i] = { track_name, artist_name, album_name, duration, has_synced, has_plain }
  for i, r in ipairs(results) do
    if r.has_synced then return i end
  end
end
```

- 钩子传的是**普通 Lua table 拷贝**，可安全存进 `ctx.state` 跨帧用（不是引用）。
- 钩子出错只记日志，不影响 preset 与内置兜底。
- `on_select` 只在 `media_selection_dirty()` 为真时调用（播放器集合或状态/元数据变化），不是每帧。
- `lrclib_set_selector()` 把候选交给 `on_search`；返回 -1/越界则回退内置启发式。

### 7.11 歌词缓存

`src/lyrics/cache.{h,cpp}`（C++ 接口，仅 main 使用）。目录默认
`$XDG_CACHE_HOME/raylyrics/lyrics`（否则 `~/.cache/raylyrics/lyrics`）。

- 每条目两个文件：`<key>.lrc`（歌词）+ `<key>.json`（artist/title/album/duration/source/time）。
- `key` = FNV-1a 64 of `artist\x1f title\x1f album\x1f duration_seconds`（时长取整秒），输出 16 位 hex。
- 查找顺序：本地 `.lrc` → 缓存 → LRCLIB；LRCLIB 命中后写入缓存。
- key 用的是 `on_metadata` 归一化 + `SplitCombinedTitle` 之后的字段，保证查找与写入一致。
- **TTL**：`config.lyrics.cache_ttl_days`（默认 30，`<= 0` 关闭）。`Get` 惰性过期——命中前用
  `.lrc` 的 mtime 判断，过期则删除并重新联网；`cache prune` 批量清理。
- `list` 读 `.json` 元数据、stat 对应 `.lrc`（缺 `.lrc` 的孤儿元数据跳过），按 artist/title 排序，
  并显示条目年龄。

## 8. 依赖
系统：`wayland-client`、`wayland-egl`、`egl`、`gl`、`fontconfig`、`gio-2.0`、`glib-2.0`、
`libsoup-3.0`、`libcjson`、`lua5.4`、`iconv`（glibc）、`wayland-scanner`（1.26）。

内置：raylib（fork）、sol2、slug-raylib、`wlr-layer-shell-unstable-v1.xml`。

## 9. 开发阶段

1. **Phase 1** ✅：raylib fork + 自定义平台 + layer-shell + 透明 overlay + 固定字符串。
2. **Phase 2** ✅：接入 slug-raylib（GL 4.3）：CFF/OTF 三次曲线转换、fontconfig 字体回退、多行、
   逐行居中、按 cluster 上色的绘制接口。不做：HarfBuzz shaping、彩色 emoji。
3. **Phase 3** ✅：GDBus + MPRIS。Firefox 优先、忽略 playerctld、`GetAll` + `PropertiesChanged` +
   `Seeked` + `NameOwnerChanged`、monotonic position 校准。`--mpris` dump 模式已验证。
4. **Phase 4** ✅：LRC 解析（标准 + 增强 + offset）、二分查找当前行；LRCLIB 在线后端
   （libsoup 异步 + cJSON）。`--lyrics` / `--lrc` 模式已验证。
5. **Phase 5** ✅：整合完成——overlay 显示当前行（整行亮）+ 下一行（暗），间奏空行清屏；
   `TextRenderer::Prepare()` 全曲预建字体一次、`SetText()` 按行只做布局，换行不再重建 SlugFont/SSBO。
6. **Phase 6** ✅：多播放器 + 逻辑钩子——`media` 跟踪所有 MPRIS 播放器并订阅其事件，
   `config.on_select` 选活动播放器，`config.on_metadata` 归一化匹配字段，
   `config.on_search` 挑选 LRCLIB 搜索结果；preset 支持自定义 shader 的 Lua uniform 注入。
   已验证：两播放器同时在线、三个钩子均生效。
7. **Phase 7** ✅：歌词磁盘缓存（`src/lyrics/cache.*`，本地 → 缓存 → LRCLIB）+
   `raylyrics cache list|clear|remove <key>|dir`。已验证缓存命中与三个管理命令。

## 10. 风险与待确认

- **GL 4.3 via EGL on KWin**：已验证可用（Mesa 26.2.2 / AMD，GL 4.6 core）。
- **configure 阻塞**：`InitPlatform` 里的事件顺序若错会死锁；已用 bounded roundtrip 处理。
- **sol2 与 `-fno-exceptions`/RTTI**：需实测；不满足则改手写绑定层。
- **slug-raylib 三次曲线精度**：每个三次段拆 2 段二次；若大字号仍有误差，可提高细分。
- **无 shaping**：CJK/拉丁可用，Devanagari/Arabic 会错位；需要时接入 HarfBuzz。
- **bloom**：downsample→upsample 金字塔已可用；关键是 FBO 纹理必须 `TEXTURE_FILTER_BILINEAR`，且加法混合用 `BLEND_ADD_COLORS` 时 `amount` 要缩放 RGB（`GL_ONE,GL_ONE` 忽略源 alpha）。
- **彩色 emoji**：CBDT 位图字体无轮廓，Slug 无法渲染，需独立纹理路径。
- **TextRenderer 重建开销**：换行即重建字体与 SSBO；Phase 5 优化项。- **本地 `.lrc` 编码**：目前按 UTF-8 读；GB18030 探测（iconv）尚未实现。
- **Firefox/Spotify MPRIS 怪癖**：`xesam:artist` 常为 `[""]`，歌名与歌手塞在 `xesam:title` 里用
  `" • "` 分隔，匹配时需拆分（已处理）。
- **fork raylib 使仓库变大**：可改用 `git subtree` 保留上游同步能力。
- **premultiplied alpha**：错误设置会导致文字边缘光晕。
- **layer-shell 版本**：KWin 报 v5，绑定 v4；协议 XML 需 vendor（系统无 wlr-protocols）。
