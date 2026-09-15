-- raylyrics configuration
-- Copy to ~/.config/raylyrics/config.lua and edit.
config = {
  overlay = {
    anchor = "bottom",      -- "bottom" | "top"
    output = "",            -- wl_output name (e.g. "DP-1"); "" = compositor default
    margin_top = 0,
    margin_right = 0,
    margin_bottom = 48,
    margin_left = 0,
    width = 800,
    height = 160,
  },

  fps = 60,

  font = {
    families = { "Noto Sans CJK SC", "Noto Sans", "DejaVu Sans" },
    size = 36,
    line_spacing = 10,
    letter_spacing = 0,
  },

  colors = {
    current = { 1, 1, 1, 1 },
    next    = { 1, 1, 1, 0.3 },
  },

  lyrics = {
    root = "~/.local/share/raylyrics/lyrics",
    cache_ttl_days = 30,   -- cached remote lyrics expire after this; <= 0 disables
  },

  -- Arbitrary values readable from presets as `config.preset_params.<key>`.
  preset_params = {
    bloom_amount = 0.45,
  },

  -- Optional logic hooks (uncomment to enable). See docs/PLAN.md 7.10.
  --
  -- Pick the active player: return a bus name or 1-based index; nil = built-in
  -- policy (playing player, Firefox first, else the first).
  -- on_select = function(players)
  --   for i, p in ipairs(players) do
  --     if p.playing and p.identity == "Spotify" then return i end
  --   end
  -- end,
  --
  -- Normalize metadata before local/LRCLIB matching (m = {artist,title,album,duration,player}).
  -- on_metadata = function(m)
  --   local artist, title = m.title:match("^(.-) • (.*)$")
  --   if artist then return { artist = artist, title = title } end
  -- end,
  --
  -- Pick among LRCLIB /api/search candidates (results[i] = {track_name,artist_name,
  -- album_name,duration,has_synced,has_plain}); return a 1-based index, or nil.
  -- on_search = function(query, results)
  --   for i, r in ipairs(results) do
  --     if r.has_synced then return i end
  --   end
  -- end,

  preset = "default",
}
