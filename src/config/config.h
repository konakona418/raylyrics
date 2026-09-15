#ifndef RAYLYRICS_CONFIG_CONFIG_H
#define RAYLYRICS_CONFIG_CONFIG_H

#include <string>
#include <vector>

namespace raylyrics {

struct OverlayConfig {
    std::string anchor = "bottom";  // "top" | "bottom"
    std::string output;             // wl_output name, empty = compositor default
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 48;
    int margin_left = 0;
    int width = 800;
    int height = 160;
};

struct FontConfig {
    std::vector<std::string> families = {"Noto Sans CJK SC", "Noto Sans", "DejaVu Sans"};
    float size = 36.0f;
    float line_spacing = 10.0f;
    float letter_spacing = 0.0f;
};

struct ColorConfig {
    float current[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float next[4] = {1.0f, 1.0f, 1.0f, 0.3f};
};

// Loaded from ~/.config/raylyrics/config.lua. Missing fields keep defaults.
struct Config {
    OverlayConfig overlay;
    FontConfig font;
    ColorConfig colors;
    std::string lyrics_root;
    int cache_ttl_days = 30;  // <= 0 disables cache expiry
    std::string preset = "default";
    int fps = 60;

    // Returns true if the file existed and was evaluated.
    bool Load(const std::string& path);

    static Config LoadDefault();
    static std::string DefaultPath();
};

}  // namespace raylyrics

#endif  // RAYLYRICS_CONFIG_CONFIG_H
