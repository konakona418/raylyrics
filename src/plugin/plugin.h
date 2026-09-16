#ifndef RAYLYRICS_PLUGIN_PLUGIN_H
#define RAYLYRICS_PLUGIN_PLUGIN_H

#include <cstdint>
#include <string>
#include <vector>

#include "render/text.h"

namespace raylyrics {

struct PluginLine {
    std::string text;
    int64_t time_us = 0;
};

// Per-frame data handed to the preset's hooks.
struct PluginContext {
    int64_t position_us = 0;
    double dt = 0.0;
    double wall_time = 0.0;
    bool seeked = false;

    int line_index = -1;
    int line_count = 0;
    std::string line_text;
    std::string next_text;
    int64_t line_time_us = 0;
    int64_t line_end_us = 0;

    std::vector<PluginLine> lines;
};

// One entry per MPRIS player, passed to config.on_select.
struct PluginPlayer {
    std::string name;
    std::string identity;
    std::string title;
    std::string artist;
    std::string album;
    bool playing = false;
    int64_t position_us = 0;
    int64_t length_us = 0;
};

// Track metadata, passed to config.on_metadata and normalized in place.
struct PluginMetadata {
    std::string artist;
    std::string title;
    std::string album;
    int64_t duration_us = 0;
    std::string player;
};

// One LRCLIB search candidate, passed to config.on_search.
struct PluginSearchResult {
    std::string track_name;
    std::string artist_name;
    std::string album_name;
    double duration = 0.0;
    bool has_synced = false;
    bool has_plain = false;
};

// Declarative settings a preset may return alongside its hooks. Only the fields
// the preset actually declares are applied; everything else keeps the config
// value. The bootstrap fields (viewport/layer/anchor/margin/output/namespace/
// exclusive_zone/keyboard) must be read before InitWindow() and are ignored on
// hot reload; fps/font/colors can be applied at runtime.
struct PresetSetup {
    bool has_viewport = false;
    int viewport_width = 0;   // <= 0 means "full output"
    int viewport_height = 0;

    bool has_layer = false;
    int layer = 3;            // matches RL_WL_LAYER_* (background..overlay)

    bool has_anchor = false;
    std::string anchor;       // "bottom", "top-left", "fullscreen", ...

    bool has_margin = false;
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 0;
    int margin_left = 0;

    bool has_output = false;
    std::string output;

    bool has_namespace = false;
    std::string layer_namespace;

    bool has_exclusive_zone = false;
    int exclusive_zone = -1;

    bool has_keyboard = false;
    bool keyboard = false;

    // Left-button dragging. Gives up click-through: the surface takes input.
    bool has_draggable = false;
    bool draggable = false;

    bool has_fps = false;
    int fps = 60;

    bool has_font = false;
    TextStyle font;

    bool has_colors = false;
    float colors_current[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float colors_next[4] = {1.0f, 1.0f, 1.0f, 0.3f};
};

// Loads and runs a Lua preset. Owns the TextRenderer so fonts survive across
// line changes (PrepareText rebuilds them per song).
class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    void SetStyle(const TextStyle& style);
    void SetColors(const float current[4], const float next[4]);
    void PrepareText(const std::string& all_song_text);

    // Evaluate the config file inside the preset Lua state so presets can read
    // the global `config` table (e.g. config.preset_params).
    void LoadConfig(const std::string& path);

    // What the current preset declared in its returned table. Valid after
    // SetPreset().
    const PresetSetup& setup() const;

    // Surface-space rectangle the preset asked to accept pointer input this
    // frame (f:input_region), or false when it asked for none.
    bool input_rect(float* x, float* y, float* w, float* h) const;

    // Bumped on every hot reload, so callers can re-apply setup.
    uint64_t reload_serial() const;

    // config.on_select(players) -> bus name / 1-based index / nil.
    // Returns "" when there is no hook or it made no choice.
    std::string SelectPlayer(const std::vector<PluginPlayer>& players);

    // config.on_metadata(meta) -> normalized {artist,title,album,duration}.
    void NormalizeMetadata(PluginMetadata& meta);

    // config.on_search(query, results) -> 1-based index / nil.
    // Returns -1 when there is no hook or it made no choice.
    int SelectSearchResult(const PluginMetadata& query,
                           const std::vector<PluginSearchResult>& results);

    // name == a file in <config>/presets, or a path, or empty for the built-in.
    void SetPreset(const std::string& name_or_path);

    // Must be called inside BeginDrawing()/EndDrawing().
    void RunFrame(const PluginContext& ctx);

    bool using_default() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_PLUGIN_PLUGIN_H
