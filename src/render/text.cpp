#include "render/text.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "raylib.h"
#include "rlgl.h"
#include "slug.h"

namespace raylyrics {

namespace {

// fontconfig gives us a file path plus a face index (for .ttc collections).
std::string ResolveFontKey(int codepoint, const std::vector<std::string>& families) {
    FcPattern* pattern = FcPatternCreate();
    FcCharSet* charset = FcCharSetCreate();
    FcCharSetAddChar(charset, static_cast<FcChar32>(codepoint));

    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    for (const std::string& family : families) {
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    }

    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);

    std::string key;
    if (match != nullptr) {
        FcChar8* file = nullptr;
        int index = 0;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
            FcPatternGetInteger(match, FC_INDEX, 0, &index);
            key = std::string(reinterpret_cast<const char*>(file)) + "#" + std::to_string(index);
        }
        FcPatternDestroy(match);
    }

    FcCharSetDestroy(charset);
    FcPatternDestroy(pattern);
    return key;
}

std::pair<std::string, int> SplitFontKey(const std::string& key) {
    const std::size_t pos = key.rfind('#');
    if (pos == std::string::npos) return {key, 0};
    return {key.substr(0, pos), std::stoi(key.substr(pos + 1))};
}

}  // namespace

struct TextRenderer::Impl {
    struct FontEntry {
        std::string key;
        PSlugFont font = nullptr;
        float scale = 1.0f;
        float ascent = 0.0f;
    };

    struct Placement {
        int font;       // index into fonts
        int codepoint;
        int cluster;    // index into codepoints
        int line;       // line index
        float x;        // pen x, relative to the line origin
        float baseline; // baseline y, relative to text block origin
    };

    // Font set (built by Prepare, reused across SetText calls).
    std::vector<FontEntry> fonts;
    std::unordered_map<int, int> codepoint_to_font;  // -1 == no glyph anywhere
    float size = 0.0f;
    float line_spacing = 0.0f;
    float letter_spacing = 0.0f;
    float primary_ascent = 0.0f;

    // Layout (rebuilt by SetText).
    std::vector<int> codepoints;  // source text, includes '\n'
    std::vector<Placement> placements;
    std::vector<float> line_widths;
    float width = 0.0f;
    float height = 0.0f;
    float line_height = 0.0f;
    int line_count = 0;
};

TextRenderer::TextRenderer() : impl_(new Impl()) {}

TextRenderer::~TextRenderer() {
    Clear();
    delete impl_;
}

void TextRenderer::ClearLayout() {
    impl_->codepoints.clear();
    impl_->placements.clear();
    impl_->line_widths.clear();
    impl_->width = 0.0f;
    impl_->height = 0.0f;
    impl_->line_height = 0.0f;
    impl_->line_count = 0;
}

void TextRenderer::Clear() {
    for (Impl::FontEntry& entry : impl_->fonts) {
        if (entry.font) UnloadFontSlug(entry.font);
    }
    impl_->fonts.clear();
    impl_->codepoint_to_font.clear();
    impl_->size = 0.0f;
    impl_->line_spacing = 0.0f;
    impl_->letter_spacing = 0.0f;
    impl_->primary_ascent = 0.0f;
    ClearLayout();
}

void TextRenderer::Prepare(const std::string& all_text, const TextStyle& style) {
    Clear();

    impl_->size = style.size;
    impl_->line_spacing = style.line_spacing;
    impl_->letter_spacing = style.letter_spacing;

    int count = 0;
    int* raw = LoadCodepoints(all_text.c_str(), &count);
    std::set<int> unique;
    if (raw != nullptr && count > 0) {
        for (int i = 0; i < count; i++) {
            if (raw[i] != '\n') unique.insert(raw[i]);
        }
        UnloadCodepoints(raw);
    }

    std::unordered_map<std::string, std::vector<int>> groups;
    std::unordered_map<int, std::string> codepoint_key;
    for (int codepoint : unique) {
        std::string key = ResolveFontKey(codepoint, style.families);
        if (key.empty()) continue;
        groups[key].push_back(codepoint);
        codepoint_key[codepoint] = key;
    }

    std::unordered_map<std::string, int> key_to_index;
    for (const auto& [key, codepoints] : groups) {
        const auto [file, face] = SplitFontKey(key);
        PSlugFont font = LoadFontSlugEx(file.c_str(), face, codepoints.data(),
                                        static_cast<int>(codepoints.size()));
        if (font == nullptr) continue;
        const int index = static_cast<int>(impl_->fonts.size());
        impl_->fonts.push_back(
            Impl::FontEntry{key, font, font->GetScaleForPixelHeight(style.size), font->GetAscent()});
        key_to_index[key] = index;
    }

    for (const auto& [codepoint, key] : codepoint_key) {
        const auto it = key_to_index.find(key);
        if (it != key_to_index.end()) impl_->codepoint_to_font[codepoint] = it->second;
    }
    for (auto& [codepoint, font_index] : impl_->codepoint_to_font) {
        if (font_index >= 0 && !impl_->fonts[font_index].font->HasGlyph(codepoint)) {
            font_index = -1;
        }
    }

    if (!impl_->fonts.empty()) {
        impl_->primary_ascent = impl_->fonts[0].ascent * impl_->fonts[0].scale;
    }

    TraceLog(LOG_INFO, "TEXT: prepared %d codepoints -> %d font(s)",
             static_cast<int>(unique.size()), static_cast<int>(impl_->fonts.size()));
    for (const Impl::FontEntry& entry : impl_->fonts) {
        TraceLog(LOG_INFO, "TEXT:   %s", entry.key.c_str());
    }
    for (const auto& [codepoint, font_index] : impl_->codepoint_to_font) {
        if (font_index < 0) TraceLog(LOG_WARNING, "TEXT: no glyph for U+%04X", codepoint);
    }
}

void TextRenderer::SetText(const std::string& utf8) {
    ClearLayout();

    const float size = impl_->size;
    const float line_height = size + impl_->line_spacing;

    int count = 0;
    int* raw = LoadCodepoints(utf8.c_str(), &count);
    if (raw == nullptr || count == 0) {
        if (raw != nullptr) UnloadCodepoints(raw);
        impl_->line_widths.assign(1, 0.0f);
        impl_->line_count = 1;
        impl_->line_height = line_height;
        impl_->height = size;
        return;
    }
    impl_->codepoints.assign(raw, raw + count);
    UnloadCodepoints(raw);

    float pen_x = 0.0f;
    int line = 0;
    std::vector<float> line_widths;

    for (int i = 0; i < static_cast<int>(impl_->codepoints.size()); i++) {
        const int codepoint = impl_->codepoints[i];
        if (codepoint == '\n') {
            line_widths.push_back(pen_x);
            pen_x = 0.0f;
            line++;
            continue;
        }

        const auto it = impl_->codepoint_to_font.find(codepoint);
        if (it == impl_->codepoint_to_font.end() || it->second < 0) continue;

        const int font_index = it->second;
        const Impl::FontEntry& entry = impl_->fonts[font_index];
        const float baseline = impl_->primary_ascent + static_cast<float>(line) * line_height;

        impl_->placements.push_back(Impl::Placement{font_index, codepoint, i, line, pen_x, baseline});

        const int next = (i + 1 < static_cast<int>(impl_->codepoints.size()) &&
                          impl_->codepoints[i + 1] != '\n')
                             ? impl_->codepoints[i + 1]
                             : 0;
        pen_x += entry.font->GetAdvance(codepoint, next) * entry.scale + impl_->letter_spacing;
    }
    line_widths.push_back(pen_x);

    impl_->line_widths = std::move(line_widths);
    impl_->width = *std::max_element(impl_->line_widths.begin(), impl_->line_widths.end());
    impl_->line_count = static_cast<int>(impl_->line_widths.size());
    impl_->line_height = line_height;
    impl_->height = static_cast<float>(impl_->line_count) * size +
                    static_cast<float>(impl_->line_count - 1) * impl_->line_spacing;
}

bool TextRenderer::Empty() const { return impl_->placements.empty(); }

int TextRenderer::LineCount() const { return impl_->line_count; }

int TextRenderer::ClusterCount() const { return static_cast<int>(impl_->codepoints.size()); }

float TextRenderer::Width() const { return impl_->width; }

float TextRenderer::Height() const { return impl_->height; }

float TextRenderer::LineWidth(int line) const {
    if (line < 0 || line >= static_cast<int>(impl_->line_widths.size())) return 0.0f;
    return impl_->line_widths[line];
}

void TextRenderer::DrawInternal(float x, float y, int start_cluster, int end_cluster,
                                Color base, Color highlight, bool center) const {
    for (const auto& placement : impl_->placements) {
        const bool in_range = placement.cluster >= start_cluster && placement.cluster < end_cluster;
        const Color tint = in_range ? highlight : base;
        const auto& entry = impl_->fonts[placement.font];
        const float ox = center ? x - impl_->line_widths[placement.line] * 0.5f : x;
        DrawTextCodepointSlug_Impl(entry.font, placement.codepoint,
                                   {ox + placement.x, y + placement.baseline},
                                   {entry.scale, entry.scale}, tint);
    }
}

void TextRenderer::Draw(float x, float y, Color color, bool center) const {
    DrawInternal(x, y, 0, 0, color, color, center);
}

void TextRenderer::DrawHighlighted(float x, float y, int start_cluster, int end_cluster,
                                   Color base, Color highlight, bool center) const {
    DrawInternal(x, y, start_cluster, end_cluster, base, highlight, center);
}

void TextRenderer::DrawLines(float x, float y, int first_line, int last_line, int hl_start,
                             int hl_end, Color base, Color highlight, bool center) const {
    for (const auto& placement : impl_->placements) {
        if (placement.line < first_line || placement.line > last_line) continue;

        const bool in_range = placement.cluster >= hl_start && placement.cluster < hl_end;
        const Color tint = in_range ? highlight : base;
        const auto& entry = impl_->fonts[placement.font];
        const float ox = center ? x - impl_->line_widths[placement.line] * 0.5f : x;
        const float oy = y + placement.baseline - static_cast<float>(first_line) * impl_->line_height;
        DrawTextCodepointSlug_Impl(entry.font, placement.codepoint, {ox + placement.x, oy},
                                   {entry.scale, entry.scale}, tint);
    }
}

float TextRenderer::LineHeight() const { return impl_->line_height; }

int TextRenderer::GlyphCount() const { return static_cast<int>(impl_->placements.size()); }

TextRenderer::GlyphMetrics TextRenderer::GlyphAt(int index) const {
    if (index < 0 || index >= static_cast<int>(impl_->placements.size())) return {};
    const Impl::Placement& placement = impl_->placements[index];
    return GlyphMetrics{placement.codepoint, placement.cluster, placement.line, placement.x,
                        placement.baseline};
}

void TextRenderer::DrawGlyph(int index, float origin_x, float origin_y, float dx, float dy,
                             float scale_mul, float rotation, Color tint) const {
    if (index < 0 || index >= static_cast<int>(impl_->placements.size())) return;
    const Impl::Placement& placement = impl_->placements[index];
    const Impl::FontEntry& entry = impl_->fonts[placement.font];

    rlPushMatrix();
    rlTranslatef(origin_x + placement.x + dx, origin_y + placement.baseline + dy, 0.0f);
    if (rotation != 0.0f) rlRotatef(rotation, 0.0f, 0.0f, 1.0f);
    rlScalef(entry.scale * scale_mul, entry.scale * scale_mul, 1.0f);
    DrawTextCodepointSlug_Impl(entry.font, placement.codepoint, {0.0f, 0.0f}, {1.0f, 1.0f}, tint);
    rlPopMatrix();
}

}  // namespace raylyrics
