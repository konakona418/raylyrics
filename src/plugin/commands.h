#ifndef RAYLYRICS_PLUGIN_COMMANDS_H
#define RAYLYRICS_PLUGIN_COMMANDS_H

#include <string>
#include <vector>

namespace raylyrics {

enum class CmdKind {
    Text,
    Rect,
    Line,
    Circle,
    Triangle,
    Image,
    Push,
    Pop,
    Translate,
    Rotate,
    Scale,
    LayerBegin,
};

struct Uniform {
    std::string name;
    int count = 1;  // 1..4
    bool is_int = false;
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Per-glyph override produced by the optional f:text callback.
struct GlyphOverride {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float scale = 1.0f;
    float rotation = 0.0f;
    float alpha = 1.0f;  // multiplies the resolved color's alpha
    bool has_color = false;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct Command {
    CmdKind kind = CmdKind::Rect;

    // Geometry
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float rotation = 0.0f;
    float scale = 1.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Text
    std::string text;
    std::string anchor;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float size = 0.0f;  // 0 == use the prepared default size
    std::vector<GlyphOverride> glyphs;

    // Image
    std::string path;

    // Layer
    std::vector<std::string> post;
    std::vector<Uniform> uniforms;
    std::size_t child_begin = 0;
    std::size_t child_end = 0;
};

// Reused across frames; capacity is retained so the steady state is allocation-free.
class CommandBuffer {
public:
    void Clear() { commands_.clear(); }

    std::size_t Size() const { return commands_.size(); }

    Command& Add(CmdKind kind) {
        commands_.emplace_back();
        commands_.back().kind = kind;
        return commands_.back();
    }

    Command& At(std::size_t index) { return commands_[index]; }
    const Command& At(std::size_t index) const { return commands_[index]; }

    const std::vector<Command>& All() const { return commands_; }

private:
    std::vector<Command> commands_;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_PLUGIN_COMMANDS_H
