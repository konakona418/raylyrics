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
