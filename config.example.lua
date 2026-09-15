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
  },

  -- Arbitrary values readable from presets as `config.preset_params.<key>`.
  preset_params = {
    bloom_amount = 0.45,
  },

  preset = "default",
}
