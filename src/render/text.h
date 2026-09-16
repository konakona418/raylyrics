#ifndef RAYLYRICS_RENDER_TEXT_H
#define RAYLYRICS_RENDER_TEXT_H

#include <string>
#include <vector>

#include "raylib.h"

namespace raylyrics {

struct TextStyle {
    // Preferred font families in priority order. Per-codepoint fallback is
    // resolved with fontconfig, so missing glyphs fall through the list.
    std::vector<std::string> families;
    float size = 44.0f;
    float line_spacing = 4.0f;
    float letter_spacing = 0.0f;
};

// Slug-based text layout and drawing. Text is (re)shaped by SetText(); drawing
// never re-shapes, so per-frame karaoke updates are cheap.
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Build fonts covering every codepoint in `all_text`. Call once per song;
    // it is the expensive part (fontconfig + Slug glyph packing + SSBO upload).
    void Prepare(const std::string& all_text, const TextStyle& style);

    // Lay out `utf8` using the already-prepared fonts. Cheap; call on every
    // lyric line change.
    void SetText(const std::string& utf8);

    void Clear();

    bool Empty() const;
    int LineCount() const;
    int ClusterCount() const;   // number of codepoints in the source text
    float Width() const;
    float Height() const;
    float LineWidth(int line) const;

    // x/y is the top-left of the text block, unless `center` is set, in which
    // case x is the horizontal center and every line is centered individually.
    void Draw(float x, float y, Color color, bool center = false) const;

    // Per-glyph karaoke coloring: clusters in [start, end) use `highlight`,
    // everything else uses `base`. Every glyph is drawn exactly once.
    void DrawHighlighted(float x, float y, int start_cluster, int end_cluster,
                         Color base, Color highlight, bool center = false) const;

    // Draw only lines [first_line, last_line]. `first_line` is placed at the top
    // of the block at y. Clusters in [hl_start, hl_end) use `highlight`.
    void DrawLines(float x, float y, int first_line, int last_line, int hl_start,
                   int hl_end, Color base, Color highlight, bool center = false) const;

    float LineHeight() const;

    // Per-glyph access for the Lua plugin layer. Valid after SetText().
    struct GlyphMetrics {
        int codepoint;
        int cluster;
        int line;
        float x;         // pen x relative to the text origin
        float baseline;  // baseline y relative to the text origin
    };
    int GlyphCount() const;
    GlyphMetrics GlyphAt(int index) const;

    // Advance width of a glyph within its line, so a run of glyphs can be
    // measured (used by the word-segmentation API).
    float GlyphAdvance(int index) const;

    // Draw the index-th glyph at origin + its pen position, with an extra
    // translate/scale/rotate and tint.
    void DrawGlyph(int index, float origin_x, float origin_y, float dx, float dy,
                   float scale_mul, float rotation, Color tint) const;

private:
    void ClearLayout();
    void DrawInternal(float x, float y, int start_cluster, int end_cluster,
                      Color base, Color highlight, bool center) const;

    struct Impl;
    Impl* impl_;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_RENDER_TEXT_H
