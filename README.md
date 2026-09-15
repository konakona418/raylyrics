# raylyrics

A lightweight, native Wayland desktop lyrics overlay. It follows the currently
playing track over MPRIS and draws synced lyrics on a wlroots layer-shell
surface, using the [Slug](https://github.com/linebender/slug) GPU glyph renderer
for sharp text at any size.

Appearance is fully scriptable: a preset is a Lua file that draws primitives,
applies transforms, renders into layers with post-processing (bloom, chroma,
blur, scanline) and can register its own fragment shaders.

## How it works

```
MPRIS (D-Bus) ──▶ active player ──▶ metadata
                                      │  (normalized by config.on_metadata)
                                      ▼
                        local .lrc ─▶ cache ─▶ LRCLIB
                                      │
                                      ▼
                             parsed LRC document
                                      │
                                      ▼
        per-frame Lua preset ─▶ draw commands ─▶ raylib + Slug ─▶ layer-shell
```

The overlay is click-through (`wl_surface` input region is empty), reserves no
screen space by default (exclusive zone `-1`) and is composited with
premultiplied alpha.

## Features

- **MPRIS**, multiple players: every player on the session bus is tracked; the
  active one is chosen by `config.on_select` or a built-in policy.
- **Lyrics sources**: a local `.lrc` next to a `file://` track, a configured
  lyrics directory (`Artist - Title.lrc`), then [LRCLIB](https://lrclib.net).
- **On-disk cache** for fetched lyrics with a configurable TTL.
- **Lua presets**: primitives, transforms, layers, post-processing, custom
  fragment shaders with Lua-injected uniforms, per-glyph callbacks, hot reload.
- **Preset-declared geometry**: a preset can set its own viewport (including
  fullscreen), layer-shell layer, anchor, margins, output, fps, font and colors.
- **IPC**: a Unix socket to hide/show, switch presets, adjust the lyric offset,
  reload and quit a running instance.

## Requirements

- Wayland compositor with `wlr-layer-shell` (KWin, Hyprland, sway, ...)
- Build: CMake ≥ 3.22, Ninja, a C++20 compiler, `pkgconf`
- Libraries: `wayland-client`, `wayland-egl`, `egl`/`libglvnd`, `fontconfig`,
  `glib2` (`gio-2.0`), `libsoup3`, `cjson`, `lua54`, `iconv` (glibc)
- `wayland-scanner` and `wayland-protocols` (for `xdg-shell.xml`)

Vendored under `vendors/`: a forked **raylib** (custom layer-shell platform),
**slug-raylib** and **sol2**.

## Build

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The default build type is `Debug`; pass `-DCMAKE_BUILD_TYPE=Release` for a
release build. To install system-wide:

```sh
sudo cmake --install build          # binary + presets under <prefix>/share/raylyrics
```

## Run

```sh
./build/raylyrics                     # the overlay
./build/raylyrics --mpris             # dump MPRIS state (debug)
./build/raylyrics --lyrics ARTIST TITLE [DURATION]   # query LRCLIB and print
./build/raylyrics --lrc FILE          # parse and print an LRC file
```

## CLI

The overlay listens on `$XDG_RUNTIME_DIR/raylyrics.sock`:

```sh
raylyrics ctl hide | show | toggle
raylyrics ctl preset <name>           # switch preset at runtime
raylyrics ctl offset <seconds>        # lyric offset, applied live
raylyrics ctl reload                  # reload the current preset
raylyrics ctl quit
```

`offset` is added to the media position before the current line is resolved, so
it does not touch the `.lrc` file.

The cache can be managed without a running instance:

```sh
raylyrics cache list                  # key, artist - title, album, duration, size, age, source
raylyrics cache remove <key>
raylyrics cache prune                 # drop expired entries
raylyrics cache clear
raylyrics cache dir                   # print the cache directory
```

## Configuration

`~/.config/raylyrics/config.lua` (see `config.example.lua`). Missing fields keep
their defaults. A preset can override `overlay`, `font` and `colors` — see
[Declarations](#declarations).

```lua
config = {
  overlay = {
    anchor = "bottom",        -- "bottom" | "top" | "fullscreen" | "top-left" | ...
    output = "",              -- wl_output name (e.g. "DP-1"); "" = compositor default
    margin_top = 0, margin_right = 0, margin_bottom = 48, margin_left = 0,
    width = 800, height = 160,
  },
  fps = 60,
  font = {
    families = { "Noto Sans CJK SC", "Noto Sans", "DejaVu Sans" },
    size = 36, line_spacing = 10, letter_spacing = 0,
  },
  colors = {
    current = { 1, 1, 1, 1 },
    next    = { 1, 1, 1, 0.3 },
  },
  lyrics = {
    root = "~/.local/share/raylyrics/lyrics",
    cache_ttl_days = 30,      -- <= 0 disables cache expiry
  },
  preset = "default",
  preset_params = {},         -- free-form, readable from presets as config.preset_params
}
```

### Selection and matching hooks

These live in `config.lua` (loaded once, independent of the visual preset) and
are evaluated in the preset Lua state. Arguments are plain Lua table copies.

```lua
-- Pick the active player: return a bus name or a 1-based index; nil keeps the
-- built-in policy (playing player, Firefox first, else the first).
config.on_select = function(players)  -- players[i] = {name, identity, title, artist, album, playing, position_us, length_us}
  for i, p in ipairs(players) do
    if p.playing and p.identity == "Spotify" then return i end
  end
end

-- Normalize metadata before matching. m = {artist, title, album, duration, player}
config.on_metadata = function(m)
  local artist, title = m.title:match("^(.-) • (.*)$")   -- Firefox/Spotify quirk
  if artist then return { artist = artist, title = title } end
end

-- Pick among LRCLIB /api/search candidates. results[i] = {track_name, artist_name,
-- album_name, duration, has_synced, has_plain}; return a 1-based index, or nil.
config.on_search = function(query, results)
  for i, r in ipairs(results) do
    if r.has_synced then return i end
  end
end
```

### Lyrics sources and cache

Resolution order: **local `.lrc` → cache → LRCLIB**.

- Local: a sibling `.lrc` next to a `file://` track URL, otherwise
  `<lyrics.root>/Artist - Title.lrc`.
- LRCLIB: `GET /api/get`, falling back to `GET /api/search` on a non-200.
- Encoding: UTF-8 (with or without BOM) is preferred, otherwise UTF-16 and
  GB18030 are detected (iconv).
- Cache: `$XDG_CACHE_HOME/raylyrics/lyrics/<key>.lrc` plus a `<key>.json`
  metadata sidecar. The key hashes artist/title/album/rounded-duration, so
  lookups and writes agree. Entries older than `cache_ttl_days` are dropped on
  lookup (and by `cache prune`).

## Presets

A preset is a Lua file under `~/.config/raylyrics/presets/<name>.lua` (or
`/usr/share/raylyrics/presets/`, searched second). Select it with
`config.preset` or `raylyrics ctl preset <name>`. `presets/` in this repository
has working examples; `common.lua` is a shared module loaded with
`require("common")` (both the user and system preset directories are on
`package.path`).

### Hooks

```lua
return {
  on_frame = function(f, ctx) ... end,   -- every frame
  on_lyric = function(ctx) ... end,      -- when the current line changes
}
```

The preset's top-level code must not call `f:*`: the file is evaluated before
the window exists so it can declare its viewport (see below).

### Context (`ctx`)

| Field | Meaning |
| --- | --- |
| `position_us` | current playback position (µs, offset applied) |
| `dt`, `wall_time` | frame delta / monotonic time (seconds) |
| `seeked` | true on the frame a seek was detected |
| `line` | `{ text, index, age, progress, duration }` |
| `next` | `{ text }` |
| `lines` | `{ { text, index, time }, ... }` for the whole song |
| `index`, `count` | current line index / total line count |
| `colors` | `{ current = {r,g,b,a}, next = {r,g,b,a} }` from config |
| `state` | a persistent table, kept across frames (reset on reload) |

### Drawing (`f`)

| Call | Notes |
| --- | --- |
| `f:text(str, {anchor, offset_x, offset_y, size, color}, glyph_cb)` | `size` is currently ignored; use the glyph callback's `scale` |
| `f:rect{x, y, w, h, color}` | |
| `f:line{x1, y1, x2, y2, thickness, color}` | |
| `f:circle{x, y, radius, color}` | |
| `f:triangle{x, y, w, h, color}` | |
| `f:image(path, {anchor, offset_x, offset_y, w, h, color})` | |
| `f:push() / f:pop() / f:translate(x, y) / f:rotate(deg) / f:scale(s)` | matrix stack |
| `f:measure(str)` | `{ width, height, line_height, line_count, lines }` (`lines` = per-line widths) |
| `f:width`, `f:height` | surface size |
| `f:shader(name, frag) -> bool` | register a custom fullscreen fragment shader |
| `f:uniforms({...})` | ambient uniforms applied to every layer shader this frame |
| `f:layer({post, uniforms}, function(l) ... end)` | render children into an FBO, post-process, composite |

Colors are `{r, g, b, a}` with components in `0..1`. Anchors are strings such as
`"bottom-center"`, `"top-left"`, `"center"`.

### Layers and post-processing

`f:layer` renders its body into an offscreen buffer, runs each `post` effect in
order and composites the result. Built-ins: `bloom` (`amount`, `threshold`),
`chroma`, `blur`, `scanline`. Unknown effects are logged and skipped.

```lua
f:layer({ post = "bloom", uniforms = { amount = 0.6, threshold = 0.35 } }, function(l)
  l:text(ctx.line.text, { anchor = "bottom-center", offset_y = -48, color = { 1, 1, 1, 1 } })
end)
```

### Custom shaders and uniforms

`f:shader(name, source)` registers a `#version 330` fragment shader; it returns
`false` if compilation fails. `f:uniforms{...}` sets uniforms for every layer
shader that frame; per-layer `uniforms` with the same name win. Uniform values
are typed from their Lua value — number → `float`, boolean → `int`, array →
`vec2/3/4` — or declared explicitly as `{ type = "int"|"float"|"vec2"|"vec3"|"vec4", value = ... }`.
`u_resolution`, `u_time` and `u_line_progress` are injected automatically.

### Per-glyph callbacks

`f:text` accepts a callback invoked once per glyph, letting you place and style
each character individually:

```lua
f:text(text, { anchor = "top-left" }, function(i, g)   -- i: 0-based glyph index
  -- g = { x, y, codepoint, cluster, line }; x/y are relative to the text origin
  return { offset_x = dx, offset_y = dy, scale = 1.2, rotation = 8, alpha = 1, color = { 1, 0.5, 0.2, 1 } }
end)
```

Because the anchor/offset fixes the text origin, a glyph can be placed at an
absolute position `p` with `offset_x = p.x - g.x` (and likewise for `y`). This is
how `presets/prism.lua` spreads a line across the screen.

### Declarations

A preset may return, next to its hooks, a table of settings that override
`config.lua`:

```lua
return {
  viewport = "fullscreen",           -- or { width = 1600, height = 400 }; 0/negative = output size
  layer = "overlay",                 -- overlay | top | bottom | background
  anchor = "bottom",                 -- contains "full" for all edges, else top/bottom/left/right
  margin = { top = 0, right = 0, bottom = 0, left = 0 },
  output = "DP-1",
  namespace = "raylyrics",
  exclusive_zone = -1,
  keyboard = false,
  fps = 60,
  font = { families = { "Noto Sans CJK SC" }, size = 72, line_spacing = 16, letter_spacing = 0 },
  colors = { current = { 1, 1, 1, 1 }, next = { 1, 1, 1, 0.3 } },
  on_frame = function(f, ctx) ... end,
}
```

`viewport`, `layer`, `anchor`, `margin`, `output`, `namespace`,
`exclusive_zone` and `keyboard` are read **before** the window is created, so
changing them needs a restart. `fps`, `font` and `colors` are re-applied on hot
reload and on `ctl preset` / `ctl reload`.

### Hot reload

The preset file's mtime is polled once a second; on change the custom shaders
and `ctx.state` are reset and the file is re-evaluated. A runtime error disables
the preset and falls back to a built-in default.

## Packaging (Arch Linux)

```sh
packaging/arch/build-and-install.sh              # build + sudo pacman -U
packaging/arch/build-and-install.sh --no-install # build only
```

The script tars the working tree, runs `makepkg`, then installs the resulting
`raylyrics-<ver>-<rel>-<arch>.pkg.tar.zst`. See `packaging/README.md`.

## Limitations

- **No text shaping.** There is no HarfBuzz: CJK and Latin are fine, but complex
  scripts (Devanagari, Arabic) will be misplaced. There is no per-character
  karaoke highlight and no color emoji (Slug needs outlines).
- **Firefox/Spotify MPRIS quirks**: the artist is sometimes empty and the title
  holds `Artist • Title`; use `config.on_metadata` to normalize. Firefox exposes
  two players (its own and `plasma-browser-integration`) whose status updates
  can lag each other.
- **Wayland only**, and it needs a compositor implementing `wlr-layer-shell`.
- Changing bootstrap declarations (`viewport`, `layer`, ...) requires a restart.
