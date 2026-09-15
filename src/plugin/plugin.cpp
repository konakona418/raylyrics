#include "plugin/plugin.h"

#include <sol/sol.hpp>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/commands.h"
#include "plugin/effects.h"
#include "raylib.h"
#include "rlgl.h"

namespace raylyrics {

namespace {

constexpr const char* kDefaultPreset = R"LUA(
return {
  on_frame = function(f, ctx)
    if ctx.line.text == "" then return end
    f:text(ctx.line.text, {
      anchor = "bottom-center",
      offset_y = -48,
      color = ctx.colors.current,
    })
    if ctx.next.text ~= "" then
      f:text(ctx.next.text, {
        anchor = "bottom-center",
        offset_y = -8,
        color = ctx.colors.next,
      })
    end
  end,
}
)LUA";

// Bloom pyramid shaders. Downsample does a 3x3 box blur (bright-pass on the
// first level); upsample is a plain copy that gets additively blended back up.
const char* kBloomDownsample =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_texel;\n"
    "uniform float u_threshold;\n"
    "uniform float u_first;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec4 sum = vec4(0.0);\n"
    "  for (int x = -1; x <= 1; x++)\n"
    "    for (int y = -1; y <= 1; y++)\n"
    "      sum += texture(texture0, fragTexCoord + vec2(float(x), float(y)) * u_texel);\n"
    "  sum /= 9.0;\n"
    "  if (u_first > 0.5) {\n"
    "    float luma = dot(sum.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    sum *= smoothstep(u_threshold, u_threshold + 0.25, luma);\n"
    "  }\n"
    "  finalColor = sum;\n"
    "}\n";

const char* kBloomUpsample =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() { finalColor = texture(texture0, fragTexCoord); }\n";

std::string ConfigDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/raylyrics";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.config/raylyrics";
}

// System-wide data dir (presets shipped by the package), fixed at configure time.
std::string DataDir() {
#ifdef RAYLYRICS_DATADIR
    return RAYLYRICS_DATADIR;
#else
    return "/usr/local/share/raylyrics";
#endif
}

std::string PresetPath(const std::string& name) {
    const std::string user = ConfigDir() + "/presets/" + name + ".lua";
    struct stat info;
    if (stat(user.c_str(), &info) == 0) return user;
    return DataDir() + "/presets/" + name + ".lua";
}

Color ToColor(const float rgba[4]) {
    return Color{static_cast<unsigned char>(rgba[0] * 255.0f),
                 static_cast<unsigned char>(rgba[1] * 255.0f),
                 static_cast<unsigned char>(rgba[2] * 255.0f),
                 static_cast<unsigned char>(rgba[3] * 255.0f)};
}

void ReadColor(const sol::object& object, float out[4]) {
    if (!object.is<sol::table>()) return;
    sol::table table = object.as<sol::table>();
    for (int i = 0; i < 4; i++) {
        sol::object component = table[i + 1];
        if (component.is<float>() || component.is<double>() || component.is<int>()) {
            out[i] = component.as<float>();
        }
    }
}

bool IsNumber(const sol::object& object) {
    return object.is<float>() || object.is<double>() || object.is<int>();
}

void ReadUniformValue(const sol::object& value, Uniform& uniform) {
    if (value.is<bool>()) {
        uniform.count = 1;
        uniform.is_int = true;
        uniform.value[0] = value.as<bool>() ? 1.0f : 0.0f;
        return;
    }
    if (IsNumber(value)) {
        uniform.count = 1;
        uniform.value[0] = value.as<float>();
        return;
    }
    if (!value.is<sol::table>()) return;
    sol::table table = value.as<sol::table>();

    // Explicit form: { type = "int"|"float"|"vec2"|"vec3"|"vec4", value = ... }
    sol::object type = table["type"];
    sol::object inner = table["value"];
    if (type.is<std::string>() && inner.valid()) {
        const std::string kind = type.as<std::string>();
        if (kind == "int") {
            uniform.count = 1;
            uniform.is_int = true;
            uniform.value[0] = inner.as<float>();
            return;
        }
        if (kind == "float") {
            uniform.count = 1;
            uniform.value[0] = inner.as<float>();
            return;
        }
        if (inner.is<sol::table>()) table = inner.as<sol::table>();
    }

    const int count = static_cast<int>(table.size() < 4 ? table.size() : 4);
    uniform.count = count;
    for (int c = 0; c < count; c++) {
        sol::object component = table[c + 1];
        if (IsNumber(component)) uniform.value[c] = component.as<float>();
    }
}

void ReadUniforms(const sol::table& table, std::vector<Uniform>& out) {
    table.for_each([&](const sol::object& key, const sol::object& value) {
        if (!key.is<std::string>()) return;
        Uniform uniform;
        uniform.name = key.as<std::string>();
        ReadUniformValue(value, uniform);
        out.push_back(uniform);
    });
}

// ax/ay are the anchor fractions: 0 = start, 0.5 = center, 1 = end.
void ResolveAnchor(const std::string& anchor, float* ax, float* ay) {
    *ax = 0.5f;
    *ay = 1.0f;
    if (anchor.find("left") != std::string::npos) {
        *ax = 0.0f;
    } else if (anchor.find("right") != std::string::npos) {
        *ax = 1.0f;
    }
    if (anchor.find("top") != std::string::npos) {
        *ay = 0.0f;
    } else if (anchor.find("bottom") != std::string::npos) {
        *ay = 1.0f;
    } else if (anchor.find("center") != std::string::npos) {
        *ay = 0.5f;
    }
}

}  // namespace

struct PluginHost::Impl {
    sol::state lua;
    TextRenderer text;
    CommandBuffer commands;

    TextStyle style;
    float current_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float next_color[4] = {1.0f, 1.0f, 1.0f, 0.3f};

    sol::table frame;
    sol::protected_function on_frame;
    sol::protected_function on_lyric;
    sol::protected_function on_metadata;
    sol::table state;
    sol::table colors;

    std::unordered_map<std::string, Texture2D> textures;

    EffectManager effects;
    RenderTexture2D fbo_a{};
    RenderTexture2D fbo_b{};
    int fbo_width = 0;
    int fbo_height = 0;
    float frame_time = 0.0f;
    float frame_resolution[2] = {0.0f, 0.0f};
    float frame_progress = 0.0f;
    // Uniforms set via f:uniforms(), applied to every layer shader this frame.
    std::vector<Uniform> ambient_uniforms;

    static constexpr int kBloomLevels = 4;
    Shader bloom_downsample{};
    Shader bloom_upsample{};
    bool bloom_shaders_ready = false;
    RenderTexture2D bloom_fbos[kBloomLevels]{};
    int bloom_fbo_w[kBloomLevels] = {0};
    int bloom_fbo_h[kBloomLevels] = {0};
    RenderTexture2D bloom_result{};
    int bloom_result_w = 0;
    int bloom_result_h = 0;

    bool failed = false;
    bool is_default = true;
    PresetSetup setup;
    uint64_t reload_serial = 0;
    int last_line_index = -2;
    uint64_t last_generation = ~0ull;
    std::string preset_path;
    long preset_mtime = 0;
    double last_reload_check = 0.0;

    Impl() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table,
                           sol::lib::package);
        state = lua.create_table();
        BuildFrameTable();

        // Let presets share code with require("common").
        sol::object current_path = lua["package"]["path"];
        std::string package_path = ConfigDir() + "/presets/?.lua";
        package_path += ";" + DataDir() + "/presets/?.lua";
        if (current_path.is<std::string>()) {
            package_path += ";" + current_path.as<std::string>();
        }
        lua["package"]["path"] = package_path;
    }

    ~Impl() {
        for (auto& [path, texture] : textures) {
            (void)path;
            UnloadTexture(texture);
        }
        if (fbo_a.id != 0) UnloadRenderTexture(fbo_a);
        if (fbo_b.id != 0) UnloadRenderTexture(fbo_b);
        for (int i = 0; i < kBloomLevels; i++) {
            if (bloom_fbos[i].id != 0) UnloadRenderTexture(bloom_fbos[i]);
        }
        if (bloom_result.id != 0) UnloadRenderTexture(bloom_result);
        if (bloom_shaders_ready) {
            UnloadShader(bloom_downsample);
            UnloadShader(bloom_upsample);
        }
    }

    void EnsureFbos(int width, int height) {
        if (fbo_a.id != 0 && fbo_width == width && fbo_height == height) return;
        if (fbo_a.id != 0) UnloadRenderTexture(fbo_a);
        if (fbo_b.id != 0) UnloadRenderTexture(fbo_b);
        fbo_a = LoadRenderTexture(width, height);
        fbo_b = LoadRenderTexture(width, height);
        SetTextureFilter(fbo_a.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(fbo_b.texture, TEXTURE_FILTER_BILINEAR);
        fbo_width = width;
        fbo_height = height;
    }

    static void SetUniform(Shader& shader, const Uniform& uniform) {
        const int uloc = GetShaderLocation(shader, uniform.name.c_str());
        if (uloc < 0) return;
        switch (uniform.count) {
            case 1:
                SetShaderValue(shader, uloc, &uniform.value[0],
                               uniform.is_int ? SHADER_UNIFORM_INT : SHADER_UNIFORM_FLOAT);
                break;
            case 2:
                SetShaderValue(shader, uloc, uniform.value, SHADER_UNIFORM_VEC2);
                break;
            case 3:
                SetShaderValue(shader, uloc, uniform.value, SHADER_UNIFORM_VEC3);
                break;
            default:
                SetShaderValue(shader, uloc, uniform.value, SHADER_UNIFORM_VEC4);
                break;
        }
    }

    void SetPostUniforms(Shader& shader, const Command& cmd) {
        int location = GetShaderLocation(shader, "u_resolution");
        if (location >= 0) SetShaderValue(shader, location, frame_resolution, SHADER_UNIFORM_VEC2);
        location = GetShaderLocation(shader, "u_time");
        if (location >= 0) SetShaderValue(shader, location, &frame_time, SHADER_UNIFORM_FLOAT);
        location = GetShaderLocation(shader, "u_line_progress");
        if (location >= 0) SetShaderValue(shader, location, &frame_progress, SHADER_UNIFORM_FLOAT);

        for (const Uniform& uniform : ambient_uniforms) SetUniform(shader, uniform);
        for (const Uniform& uniform : cmd.uniforms) SetUniform(shader, uniform);
    }

    void CompositeTexture(Texture2D texture, int width, int height) {
        BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
        DrawTexturePro(texture,
                       Rectangle{0, 0, static_cast<float>(texture.width),
                                 -static_cast<float>(texture.height)},
                       Rectangle{0, 0, static_cast<float>(width), static_cast<float>(height)},
                       Vector2{0, 0}, 0.0f, WHITE);
        EndBlendMode();
    }

    RenderTexture2D& PickTarget(Texture2D current) {
        if (fbo_a.texture.id != current.id) return fbo_a;
        return fbo_b;
    }

    void EnsureBloomShaders() {
        if (bloom_shaders_ready) return;
        bloom_downsample = LoadShaderFromMemory(nullptr, kBloomDownsample);
        bloom_upsample = LoadShaderFromMemory(nullptr, kBloomUpsample);
        bloom_shaders_ready = true;
    }

    void EnsureBloomFbo(int level, int width, int height) {
        if (bloom_fbos[level].id != 0 && bloom_fbo_w[level] == width && bloom_fbo_h[level] == height) {
            return;
        }
        if (bloom_fbos[level].id != 0) UnloadRenderTexture(bloom_fbos[level]);
        bloom_fbos[level] = LoadRenderTexture(width, height);
        SetTextureFilter(bloom_fbos[level].texture, TEXTURE_FILTER_BILINEAR);
        bloom_fbo_w[level] = width;
        bloom_fbo_h[level] = height;
    }

    void EnsureBloomResult(int width, int height) {
        if (bloom_result.id != 0 && bloom_result_w == width && bloom_result_h == height) return;
        if (bloom_result.id != 0) UnloadRenderTexture(bloom_result);
        bloom_result = LoadRenderTexture(width, height);
        SetTextureFilter(bloom_result.texture, TEXTURE_FILTER_BILINEAR);
        bloom_result_w = width;
        bloom_result_h = height;
    }

    // Downsample to a small pyramid, blur, then additively upsample back.
    Texture2D ApplyBloom(Texture2D source, int width, int height, const Command& cmd) {
        EnsureBloomShaders();

        float threshold = 0.6f;
        float amount = 0.6f;
        for (const Uniform& uniform : ambient_uniforms) {
            if (uniform.name == "threshold") threshold = uniform.value[0];
            else if (uniform.name == "amount") amount = uniform.value[0];
        }
        for (const Uniform& uniform : cmd.uniforms) {
            if (uniform.name == "threshold") threshold = uniform.value[0];
            else if (uniform.name == "amount") amount = uniform.value[0];
        }

        // Each upsample level contributes a fraction, otherwise the additive
        // pyramid blows out the glow.
        constexpr float kLevelWeight = 0.7f;

        Texture2D src = source;
        int src_w = width;
        int src_h = height;

        for (int level = 0; level < kBloomLevels; level++) {
            const int dst_w = src_w / 2 > 1 ? src_w / 2 : 1;
            const int dst_h = src_h / 2 > 1 ? src_h / 2 : 1;
            EnsureBloomFbo(level, dst_w, dst_h);

            BeginTextureMode(bloom_fbos[level]);
            ClearBackground(BLANK);
            BeginShaderMode(bloom_downsample);
            const float texel[2] = {1.0f / static_cast<float>(src_w),
                                    1.0f / static_cast<float>(src_h)};
            int location = GetShaderLocation(bloom_downsample, "u_texel");
            if (location >= 0) SetShaderValue(bloom_downsample, location, texel, SHADER_UNIFORM_VEC2);
            location = GetShaderLocation(bloom_downsample, "u_threshold");
            if (location >= 0) {
                SetShaderValue(bloom_downsample, location, &threshold, SHADER_UNIFORM_FLOAT);
            }
            const float first = (level == 0) ? 1.0f : 0.0f;
            location = GetShaderLocation(bloom_downsample, "u_first");
            if (location >= 0) SetShaderValue(bloom_downsample, location, &first, SHADER_UNIFORM_FLOAT);

            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            DrawTexturePro(src,
                           Rectangle{0, 0, static_cast<float>(src_w), -static_cast<float>(src_h)},
                           Rectangle{0, 0, static_cast<float>(dst_w), static_cast<float>(dst_h)},
                           Vector2{0, 0}, 0.0f, WHITE);
            EndBlendMode();
            EndShaderMode();
            EndTextureMode();

            src = bloom_fbos[level].texture;
            src_w = dst_w;
            src_h = dst_h;
        }

        for (int level = kBloomLevels - 2; level >= 0; level--) {
            const int dst_w = bloom_fbos[level].texture.width;
            const int dst_h = bloom_fbos[level].texture.height;
            const auto weight = static_cast<unsigned char>(kLevelWeight * 255.0f);
            const Color upsample_tint = {weight, weight, weight, 255};
            BeginTextureMode(bloom_fbos[level]);
            BeginBlendMode(BLEND_ADD_COLORS);
            BeginShaderMode(bloom_upsample);
            DrawTexturePro(bloom_fbos[level + 1].texture,
                           Rectangle{0, 0, static_cast<float>(bloom_fbos[level + 1].texture.width),
                                     -static_cast<float>(bloom_fbos[level + 1].texture.height)},
                           Rectangle{0, 0, static_cast<float>(dst_w), static_cast<float>(dst_h)},
                           Vector2{0, 0}, 0.0f, upsample_tint);
            EndShaderMode();
            EndBlendMode();
            EndTextureMode();
        }

        EnsureBloomResult(width, height);
        BeginTextureMode(bloom_result);
        ClearBackground(BLANK);
        BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
        DrawTexturePro(source,
                       Rectangle{0, 0, static_cast<float>(width), -static_cast<float>(height)},
                       Rectangle{0, 0, static_cast<float>(width), static_cast<float>(height)},
                       Vector2{0, 0}, 0.0f, WHITE);
        EndBlendMode();
        BeginBlendMode(BLEND_ADD_COLORS);
        const float clamped = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
        // BLEND_ADD_COLORS is (GL_ONE, GL_ONE): it ignores the source alpha, so
        // `amount` must scale RGB (the tint's alpha would have no effect).
        const auto level = static_cast<unsigned char>(clamped * 255.0f);
        const Color tint = {level, level, level, 255};
        DrawTexturePro(bloom_fbos[0].texture,
                       Rectangle{0, 0, static_cast<float>(bloom_fbos[0].texture.width),
                                 -static_cast<float>(bloom_fbos[0].texture.height)},
                       Rectangle{0, 0, static_cast<float>(width), static_cast<float>(height)},
                       Vector2{0, 0}, 0.0f, tint);
        EndBlendMode();
        EndTextureMode();

        return bloom_result.texture;
    }

    void BuildFrameTable() {
        frame = lua.create_table();
        frame["width"] = 0;
        frame["height"] = 0;

        // NOTE: presets use colon syntax (f:text(...)), so every method takes
        // the frame table as its first (ignored) argument.
        frame.set_function("push", [this](sol::table) { commands.Add(CmdKind::Push); });
        frame.set_function("pop", [this](sol::table) { commands.Add(CmdKind::Pop); });
        frame.set_function("translate", [this](sol::table, float x, float y) {
            Command& cmd = commands.Add(CmdKind::Translate);
            cmd.x = x;
            cmd.y = y;
        });
        frame.set_function("rotate", [this](sol::table, float degrees) {
            Command& cmd = commands.Add(CmdKind::Rotate);
            cmd.rotation = degrees;
        });
        frame.set_function("scale", [this](sol::table, float factor) {
            Command& cmd = commands.Add(CmdKind::Scale);
            cmd.scale = factor;
        });

        frame.set_function(
            "text",
            [this](sol::table, const std::string& value, sol::optional<sol::table> options,
                   sol::optional<sol::protected_function> glyph_callback) {
                const std::size_t index = commands.Size();
                commands.Add(CmdKind::Text);
                commands.At(index).text = value;
                commands.At(index).anchor = "bottom-center";
                if (options) {
                    sol::table table = *options;
                    commands.At(index).anchor = table["anchor"].get_or(commands.At(index).anchor);
                    commands.At(index).offset_x = table["offset_x"].get_or(0.0f);
                    commands.At(index).offset_y = table["offset_y"].get_or(0.0f);
                    commands.At(index).size = table["size"].get_or(0.0f);
                    ReadColor(table["color"], commands.At(index).color);
                }

                if (!glyph_callback) return;

                text.SetText(value);
                const int count = text.GlyphCount();
                commands.At(index).glyphs.resize(static_cast<std::size_t>(count));
                for (int i = 0; i < count; i++) {
                    const TextRenderer::GlyphMetrics metrics = text.GlyphAt(i);
                    sol::table glyph = lua.create_table();
                    glyph["x"] = metrics.x;
                    glyph["y"] = metrics.baseline;
                    glyph["codepoint"] = metrics.codepoint;
                    glyph["cluster"] = metrics.cluster;
                    glyph["line"] = metrics.line;

                    sol::protected_function_result result = (*glyph_callback)(i, glyph);
                    if (!result.valid()) {
                        Fail(result);
                        continue;
                    }
                    sol::object override_object = result;
                    if (!override_object.is<sol::table>()) continue;

                    sol::table override_table = override_object.as<sol::table>();
                    GlyphOverride& override_value = commands.At(index).glyphs[i];
                    override_value.offset_x = override_table["offset_x"].get_or(0.0f);
                    override_value.offset_y = override_table["offset_y"].get_or(0.0f);
                    override_value.scale = override_table["scale"].get_or(1.0f);
                    override_value.rotation = override_table["rotation"].get_or(0.0f);
                    override_value.alpha = override_table["alpha"].get_or(1.0f);
                    sol::object color = override_table["color"];
                    if (color.is<sol::table>()) {
                        override_value.has_color = true;
                        ReadColor(color, override_value.color);
                    }
                }
            });

        frame.set_function("measure", [this](sol::table, const std::string& value) {
            text.SetText(value);
            sol::table result = lua.create_table();
            result["width"] = text.Width();
            result["height"] = text.Height();
            result["line_height"] = text.LineHeight();
            result["line_count"] = text.LineCount();
            sol::table lines = lua.create_table();
            for (int i = 0; i < text.LineCount(); i++) {
                lines[i + 1] = text.LineWidth(i);
            }
            result["lines"] = lines;
            return result;
        });

        frame.set_function("rect", [this](sol::table, sol::table table) {
            Command& cmd = commands.Add(CmdKind::Rect);
            cmd.x = table["x"].get_or(0.0f);
            cmd.y = table["y"].get_or(0.0f);
            cmd.w = table["w"].get_or(0.0f);
            cmd.h = table["h"].get_or(0.0f);
            ReadColor(table["color"], cmd.color);
        });

        frame.set_function("line", [this](sol::table, sol::table table) {
            Command& cmd = commands.Add(CmdKind::Line);
            cmd.x = table["x1"].get_or(0.0f);
            cmd.y = table["y1"].get_or(0.0f);
            cmd.x2 = table["x2"].get_or(0.0f);
            cmd.y2 = table["y2"].get_or(0.0f);
            cmd.w = table["thickness"].get_or(1.0f);
            ReadColor(table["color"], cmd.color);
        });

        frame.set_function("circle", [this](sol::table, sol::table table) {
            Command& cmd = commands.Add(CmdKind::Circle);
            cmd.x = table["x"].get_or(0.0f);
            cmd.y = table["y"].get_or(0.0f);
            cmd.w = table["radius"].get_or(1.0f);
            ReadColor(table["color"], cmd.color);
        });

        frame.set_function("triangle", [this](sol::table, sol::table table) {
            Command& cmd = commands.Add(CmdKind::Triangle);
            cmd.x = table["x"].get_or(0.0f);
            cmd.y = table["y"].get_or(0.0f);
            cmd.w = table["w"].get_or(0.0f);
            cmd.h = table["h"].get_or(0.0f);
            ReadColor(table["color"], cmd.color);
        });

        frame.set_function(
            "image", [this](sol::table, const std::string& path, sol::optional<sol::table> options) {
                Command& cmd = commands.Add(CmdKind::Image);
                cmd.path = path;
                cmd.anchor = "top-left";
                cmd.w = 0.0f;
                cmd.h = 0.0f;
                if (options) {
                    sol::table table = *options;
                    cmd.anchor = table["anchor"].get_or(cmd.anchor);
                    cmd.offset_x = table["offset_x"].get_or(0.0f);
                    cmd.offset_y = table["offset_y"].get_or(0.0f);
                    cmd.w = table["w"].get_or(0.0f);
                    cmd.h = table["h"].get_or(0.0f);
                    ReadColor(table["color"], cmd.color);
                }
            });

        frame.set_function(
            "shader", [this](sol::table, const std::string& name, const std::string& source) {
                return effects.Register(name, source);
            });

        // Ambient shader uniforms: applied to every layer shader this frame.
        // Per-layer uniforms with the same name take precedence.
        frame.set_function("uniforms", [this](sol::table, sol::table table) {
            ambient_uniforms.clear();
            ReadUniforms(table, ambient_uniforms);
        });

        frame.set_function(
            "layer",
            [this](sol::table, sol::optional<sol::table> options, sol::protected_function body) {
                const std::size_t begin = commands.Size();
                commands.Add(CmdKind::LayerBegin);

                if (options) {
                    sol::table table = *options;
                    sol::object post = table["post"];
                    if (post.is<std::string>()) {
                        commands.At(begin).post.push_back(post.as<std::string>());
                    } else if (post.is<sol::table>()) {
                        sol::table list = post.as<sol::table>();
                        for (std::size_t i = 1; i <= list.size(); i++) {
                            sol::object entry = list[i];
                            if (entry.is<std::string>()) {
                                commands.At(begin).post.push_back(entry.as<std::string>());
                            }
                        }
                    }

                    sol::object uniforms = table["uniforms"];
                    if (uniforms.is<sol::table>()) {
                        ReadUniforms(uniforms.as<sol::table>(), commands.At(begin).uniforms);
                    }
                }

                sol::protected_function_result result = body(frame);
                // Always record the child range, even on error, so the
                // compositor cannot underflow while skipping the layer body.
                commands.At(begin).child_begin = begin + 1;
                commands.At(begin).child_end = commands.Size();
                if (!result.valid()) {
                    Fail(result);
                }
            });
    }

    void LogError(const sol::protected_function_result& result) {
        sol::error error = result;
        std::fprintf(stderr, "raylyrics: preset error: %s\n", error.what());
    }

    void Fail(const sol::protected_function_result& result) {
        LogError(result);
        std::fprintf(stderr, "raylyrics: disabling preset, falling back to default\n");
        failed = true;
    }

    void Execute(std::size_t begin, std::size_t end) {
        const float surface_width = static_cast<float>(GetScreenWidth());
        const float surface_height = static_cast<float>(GetScreenHeight());

        for (std::size_t i = begin; i < end; i++) {
            const Command& cmd = commands.At(i);
            switch (cmd.kind) {
                case CmdKind::Push:
                    rlPushMatrix();
                    break;
                case CmdKind::Pop:
                    rlPopMatrix();
                    break;
                case CmdKind::Translate:
                    rlTranslatef(cmd.x, cmd.y, 0.0f);
                    break;
                case CmdKind::Rotate:
                    rlRotatef(cmd.rotation, 0.0f, 0.0f, 1.0f);
                    break;
                case CmdKind::Scale:
                    rlScalef(cmd.scale, cmd.scale, 1.0f);
                    break;
                case CmdKind::Rect:
                    DrawRectangleRec(Rectangle{cmd.x, cmd.y, cmd.w, cmd.h}, ToColor(cmd.color));
                    break;
                case CmdKind::Line:
                    DrawLineEx(Vector2{cmd.x, cmd.y}, Vector2{cmd.x2, cmd.y2}, cmd.w,
                               ToColor(cmd.color));
                    break;
                case CmdKind::Circle:
                    DrawCircleV(Vector2{cmd.x, cmd.y}, cmd.w, ToColor(cmd.color));
                    break;
                case CmdKind::Triangle:
                    DrawTriangle(Vector2{cmd.x + cmd.w * 0.5f, cmd.y},
                                 Vector2{cmd.x + cmd.w, cmd.y + cmd.h}, Vector2{cmd.x, cmd.y + cmd.h},
                                 ToColor(cmd.color));
                    break;
                case CmdKind::Image: {
                    auto it = textures.find(cmd.path);
                    if (it == textures.end()) {
                        Texture2D texture = LoadTexture(cmd.path.c_str());
                        it = textures.emplace(cmd.path, texture).first;
                    }
                    const Texture2D& texture = it->second;
                    float w = (cmd.w > 0.0f) ? cmd.w : static_cast<float>(texture.width);
                    float h = (cmd.h > 0.0f) ? cmd.h : static_cast<float>(texture.height);
                    float ax = 0.0f, ay = 0.0f;
                    ResolveAnchor(cmd.anchor, &ax, &ay);
                    const float x = surface_width * ax - w * ax + cmd.offset_x;
                    const float y = surface_height * ay - h * ay + cmd.offset_y;
                    DrawTexturePro(texture, Rectangle{0, 0, static_cast<float>(texture.width),
                                                      static_cast<float>(texture.height)},
                                   Rectangle{x, y, w, h}, Vector2{0, 0}, 0.0f, ToColor(cmd.color));
                    break;
                }
                case CmdKind::Text: {
                    // Slug draws immediately with glDrawArrays(), but raylib
                    // shapes/textures are batched and only flushed at EndDrawing.
                    // Flush here so anything drawn before the text stays behind it.
                    rlDrawRenderBatchActive();

                    text.SetText(cmd.text);
                    const float width = text.Width();
                    const float height = text.Height();
                    float ax = 0.5f, ay = 1.0f;
                    ResolveAnchor(cmd.anchor, &ax, &ay);
                    const float x = surface_width * ax - width * ax + cmd.offset_x;
                    const float y = surface_height * ay - height * ay + cmd.offset_y;
                    const Color color = ToColor(cmd.color);

                    if (cmd.glyphs.empty()) {
                        text.DrawLines(x, y, 0, text.LineCount() - 1, 0, 0, color, color, false);
                    } else {
                        const int count = text.GlyphCount();
                        for (int g = 0; g < count; g++) {
                            const GlyphOverride& override_value = cmd.glyphs[g];
                            Color tint =
                                override_value.has_color ? ToColor(override_value.color) : color;
                            tint.a = static_cast<unsigned char>(
                                static_cast<float>(tint.a) *
                                (override_value.alpha < 0.0f ? 0.0f
                                                             : (override_value.alpha > 1.0f
                                                                    ? 1.0f
                                                                    : override_value.alpha)));
                            text.DrawGlyph(g, x, y, override_value.offset_x, override_value.offset_y,
                                           override_value.scale, override_value.rotation, tint);
                        }
                    }
                    break;
                }
                case CmdKind::LayerBegin: {
                    rlDrawRenderBatchActive();
                    const int width = GetScreenWidth();
                    const int height = GetScreenHeight();

                    if (cmd.child_end > cmd.child_begin) {
                        EnsureFbos(width, height);

                        BeginTextureMode(fbo_a);
                        ClearBackground(BLANK);
                        Execute(cmd.child_begin, cmd.child_end);
                        rlDrawRenderBatchActive();
                        EndTextureMode();

                        Texture2D current = fbo_a.texture;
                        for (const std::string& post : cmd.post) {
                            if (post == "bloom") {
                                current = ApplyBloom(current, width, height, cmd);
                                continue;
                            }

                            Shader* shader = effects.Get(post);
                            if (shader == nullptr) {
                                std::fprintf(stderr, "raylyrics: unknown effect '%s'\n",
                                             post.c_str());
                                continue;
                            }

                            RenderTexture2D& target = PickTarget(current);
                            BeginTextureMode(target);
                            ClearBackground(BLANK);
                            BeginShaderMode(*shader);
                            SetPostUniforms(*shader, cmd);
                            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
                            DrawTexturePro(current,
                                           Rectangle{0, 0, static_cast<float>(current.width),
                                                     -static_cast<float>(current.height)},
                                           Rectangle{0, 0, static_cast<float>(width),
                                                     static_cast<float>(height)},
                                           Vector2{0, 0}, 0.0f, WHITE);
                            EndBlendMode();
                            EndShaderMode();
                            EndTextureMode();

                            current = target.texture;
                        }

                        CompositeTexture(current, width, height);
                    }

                    if (cmd.child_end > i) i = cmd.child_end - 1;
                    break;
                }
            }
        }
    }

    void DrawDefault(const PluginContext& ctx) {
        const float surface_width = static_cast<float>(GetScreenWidth());
        const float surface_height = static_cast<float>(GetScreenHeight());
        const float line_height = text.LineHeight();

        if (!ctx.line_text.empty()) {
            text.SetText(ctx.line_text);
            const float x = surface_width * 0.5f - text.Width() * 0.5f;
            text.DrawLines(x, surface_height - line_height * 2.0f, 0, 0, 0, 0, ToColor(current_color),
                           ToColor(current_color), false);
        }
        if (!ctx.next_text.empty()) {
            text.SetText(ctx.next_text);
            const float x = surface_width * 0.5f - text.Width() * 0.5f;
            text.DrawLines(x, surface_height - line_height, 0, 0, 0, 0, ToColor(next_color),
                           ToColor(next_color), false);
        }
    }

    void ReadSetup(sol::table preset) {
        setup = PresetSetup{};

        sol::object viewport = preset["viewport"];
        if (viewport.is<std::string>()) {
            if (viewport.as<std::string>() == "fullscreen") {
                setup.has_viewport = true;
                setup.viewport_width = 0;
                setup.viewport_height = 0;
            }
        } else if (viewport.is<sol::table>()) {
            sol::table table = viewport.as<sol::table>();
            setup.has_viewport = true;
            setup.viewport_width = table["width"].get_or(0);
            setup.viewport_height = table["height"].get_or(0);
        }

        sol::object layer = preset["layer"];
        if (layer.is<std::string>()) {
            const std::string name = layer.as<std::string>();
            setup.has_layer = true;
            if (name == "background") {
                setup.layer = 0;
            } else if (name == "bottom") {
                setup.layer = 1;
            } else if (name == "top") {
                setup.layer = 2;
            } else {
                setup.layer = 3;
            }
        }

        sol::object anchor = preset["anchor"];
        if (anchor.is<std::string>()) {
            setup.has_anchor = true;
            setup.anchor = anchor.as<std::string>();
        }

        sol::object margin = preset["margin"];
        if (margin.is<sol::table>()) {
            sol::table table = margin.as<sol::table>();
            setup.has_margin = true;
            setup.margin_top = table["top"].get_or(0);
            setup.margin_right = table["right"].get_or(0);
            setup.margin_bottom = table["bottom"].get_or(0);
            setup.margin_left = table["left"].get_or(0);
        }

        sol::object output = preset["output"];
        if (output.is<std::string>()) {
            setup.has_output = true;
            setup.output = output.as<std::string>();
        }

        sol::object name_space = preset["namespace"];
        if (name_space.is<std::string>()) {
            setup.has_namespace = true;
            setup.layer_namespace = name_space.as<std::string>();
        }

        sol::object exclusive_zone = preset["exclusive_zone"];
        if (exclusive_zone.is<int>()) {
            setup.has_exclusive_zone = true;
            setup.exclusive_zone = exclusive_zone.as<int>();
        }

        sol::object keyboard = preset["keyboard"];
        if (keyboard.is<bool>()) {
            setup.has_keyboard = true;
            setup.keyboard = keyboard.as<bool>();
        }

        sol::object fps = preset["fps"];
        if (fps.is<int>()) {
            setup.has_fps = true;
            setup.fps = fps.as<int>();
        }

        sol::object font = preset["font"];
        if (font.is<sol::table>()) {
            sol::table table = font.as<sol::table>();
            setup.has_font = true;
            sol::object families = table["families"];
            if (families.is<sol::table>()) {
                sol::table list = families.as<sol::table>();
                setup.font.families.clear();
                for (std::size_t i = 1; i <= list.size(); i++) {
                    sol::object entry = list[i];
                    if (entry.is<std::string>()) {
                        setup.font.families.push_back(entry.as<std::string>());
                    }
                }
            }
            setup.font.size = table["size"].get_or(setup.font.size);
            setup.font.line_spacing = table["line_spacing"].get_or(setup.font.line_spacing);
            setup.font.letter_spacing = table["letter_spacing"].get_or(setup.font.letter_spacing);
        }

        sol::object colors = preset["colors"];
        if (colors.is<sol::table>()) {
            sol::table table = colors.as<sol::table>();
            setup.has_colors = true;
            ReadColor(table["current"], setup.colors_current);
            ReadColor(table["next"], setup.colors_next);
        }
    }

    void SetSource(const std::string& source) {
        sol::protected_function_result result = lua.safe_script(source, sol::script_pass_on_error);
        if (!result.valid()) {
            LogError(result);
            on_frame = sol::protected_function();
            on_lyric = sol::protected_function();
            on_metadata = sol::protected_function();
            setup = PresetSetup{};
            failed = false;
            is_default = true;
            return;
        }

        sol::object value = result;
        if (!value.is<sol::table>()) {
            std::fprintf(stderr, "raylyrics: preset must return a table\n");
            setup = PresetSetup{};
            failed = false;
            is_default = true;
            return;
        }

        sol::table preset = value.as<sol::table>();
        on_frame = preset["on_frame"];
        on_lyric = preset["on_lyric"];
        on_metadata = preset["on_metadata"];
        ReadSetup(preset);
        failed = false;
    }

    void LoadPresetFile(const std::string& path) {
        sol::protected_function_result result = lua.safe_script_file(path, sol::script_pass_on_error);
        if (!result.valid()) {
            sol::error error = result;
            std::fprintf(stderr, "raylyrics: cannot load preset %s: %s\n", path.c_str(),
                         error.what());
            is_default = true;
            SetSource(kDefaultPreset);
            return;
        }

        sol::object value = result;
        if (!value.is<sol::table>()) {
            std::fprintf(stderr, "raylyrics: preset %s must return a table\n", path.c_str());
            is_default = true;
            SetSource(kDefaultPreset);
            return;
        }

        sol::table preset = value.as<sol::table>();
        on_frame = preset["on_frame"];
        on_lyric = preset["on_lyric"];
        on_metadata = preset["on_metadata"];
        ReadSetup(preset);
        failed = false;
        is_default = false;
    }

    void ReloadPreset() {
        if (preset_path.empty()) return;
        effects.ClearCustom();
        state = lua.create_table();
        last_line_index = -2;
        LoadPresetFile(preset_path);
        reload_serial++;
        std::fprintf(stderr, "raylyrics: reloaded preset %s\n", preset_path.c_str());
    }
};

PluginHost::PluginHost() : impl_(new Impl()) {}

PluginHost::~PluginHost() { delete impl_; }

void PluginHost::SetStyle(const TextStyle& style) { impl_->style = style; }

void PluginHost::SetColors(const float current[4], const float next[4]) {
    for (int i = 0; i < 4; i++) {
        impl_->current_color[i] = current[i];
        impl_->next_color[i] = next[i];
    }
}

void PluginHost::PrepareText(const std::string& all_song_text) {
    impl_->text.Prepare(all_song_text, impl_->style);
}

void PluginHost::LoadConfig(const std::string& path) {
    // The config file assigns a global `config` table; presets read it.
    impl_->lua.safe_script_file(path, sol::script_pass_on_error);
}

namespace {

// Returns the config hook `name`, or an invalid object when absent.
sol::object ConfigHook(sol::state& lua, const char* name) {
    sol::object config = lua["config"];
    if (!config.is<sol::table>()) return sol::nil;
    return config.as<sol::table>()[name];
}

}  // namespace

std::string PluginHost::SelectPlayer(const std::vector<PluginPlayer>& players) {
    Impl& impl = *impl_;
    sol::object hook = ConfigHook(impl.lua, "on_select");
    if (!hook.is<sol::function>()) return {};

    sol::table list = impl.lua.create_table();
    for (std::size_t i = 0; i < players.size(); i++) {
        const PluginPlayer& player = players[i];
        sol::table entry = impl.lua.create_table();
        entry["name"] = player.name;
        entry["identity"] = player.identity;
        entry["title"] = player.title;
        entry["artist"] = player.artist;
        entry["album"] = player.album;
        entry["playing"] = player.playing;
        entry["position_us"] = player.position_us;
        entry["length_us"] = player.length_us;
        list[i + 1] = entry;
    }

    sol::protected_function function = hook.as<sol::protected_function>();
    sol::protected_function_result result = function(list);
    if (!result.valid()) {
        impl.LogError(result);
        return {};
    }

    sol::object value = result.get<sol::object>();
    if (value.is<std::string>()) return value.as<std::string>();
    if (value.is<int>()) {
        const int index = value.as<int>();
        if (index >= 1 && index <= static_cast<int>(players.size())) {
            return players[static_cast<std::size_t>(index - 1)].name;
        }
    }
    return {};
}

void PluginHost::NormalizeMetadata(PluginMetadata& meta) {
    Impl& impl = *impl_;
    sol::object hook = ConfigHook(impl.lua, "on_metadata");
    if (!hook.is<sol::function>()) return;

    sol::table table = impl.lua.create_table();
    table["artist"] = meta.artist;
    table["title"] = meta.title;
    table["album"] = meta.album;
    table["duration"] = static_cast<double>(meta.duration_us) / 1e6;
    table["player"] = meta.player;

    sol::protected_function function = hook.as<sol::protected_function>();
    sol::protected_function_result result = function(table);
    if (!result.valid()) {
        impl.LogError(result);
        return;
    }

    sol::object value = result.get<sol::object>();
    if (!value.is<sol::table>()) return;
    sol::table out = value.as<sol::table>();
    sol::object artist = out["artist"];
    sol::object title = out["title"];
    sol::object album = out["album"];
    sol::object duration = out["duration"];
    if (artist.is<std::string>()) meta.artist = artist.as<std::string>();
    if (title.is<std::string>()) meta.title = title.as<std::string>();
    if (album.is<std::string>()) meta.album = album.as<std::string>();
    if (duration.is<double>() || duration.is<float>() || duration.is<int>()) {
        meta.duration_us = static_cast<int64_t>(duration.as<double>() * 1e6);
    }
}

int PluginHost::SelectSearchResult(const PluginMetadata& query,
                                   const std::vector<PluginSearchResult>& results) {
    Impl& impl = *impl_;
    sol::object hook = ConfigHook(impl.lua, "on_search");
    if (!hook.is<sol::function>()) return -1;

    sol::table query_table = impl.lua.create_table();
    query_table["artist"] = query.artist;
    query_table["title"] = query.title;
    query_table["album"] = query.album;
    query_table["duration"] = static_cast<double>(query.duration_us) / 1e6;

    sol::table list = impl.lua.create_table();
    for (std::size_t i = 0; i < results.size(); i++) {
        const PluginSearchResult& candidate = results[i];
        sol::table entry = impl.lua.create_table();
        entry["track_name"] = candidate.track_name;
        entry["artist_name"] = candidate.artist_name;
        entry["album_name"] = candidate.album_name;
        entry["duration"] = candidate.duration;
        entry["has_synced"] = candidate.has_synced;
        entry["has_plain"] = candidate.has_plain;
        list[i + 1] = entry;
    }

    sol::protected_function function = hook.as<sol::protected_function>();
    sol::protected_function_result result = function(query_table, list);
    if (!result.valid()) {
        impl.LogError(result);
        return -1;
    }

    sol::object value = result.get<sol::object>();
    if (value.is<int>()) {
        const int index = value.as<int>();
        if (index >= 1 && index <= static_cast<int>(results.size())) return index - 1;
    }
    return -1;
}

void PluginHost::SetPreset(const std::string& name_or_path) {
    if (name_or_path.empty() || name_or_path == "default") {
        impl_->preset_path.clear();
        impl_->is_default = true;
        impl_->SetSource(kDefaultPreset);
        return;
    }

    std::string path = name_or_path;
    if (path.find('/') == std::string::npos) {
        path = PresetPath(path);
    }

    struct stat info;
    impl_->preset_mtime = (stat(path.c_str(), &info) == 0) ? info.st_mtime : 0;
    impl_->LoadPresetFile(path);
    if (impl_->is_default) {
        impl_->preset_path.clear();
    } else {
        impl_->preset_path = path;
        std::fprintf(stderr, "raylyrics: loaded preset %s\n", path.c_str());
    }
}

void PluginHost::RunFrame(const PluginContext& ctx) {
    Impl& impl = *impl_;

    // Hot reload: poll the preset file's mtime once a second.
    if (!impl.preset_path.empty() && ctx.wall_time - impl.last_reload_check > 1.0) {
        impl.last_reload_check = ctx.wall_time;
        struct stat info;
        if (stat(impl.preset_path.c_str(), &info) == 0 && info.st_mtime != impl.preset_mtime) {
            impl.preset_mtime = info.st_mtime;
            impl.ReloadPreset();
        }
    }

    if (impl.failed) {
        impl.DrawDefault(ctx);
        return;
    }

    impl.commands.Clear();
    impl.ambient_uniforms.clear();
    impl.frame["width"] = GetScreenWidth();
    impl.frame["height"] = GetScreenHeight();
    impl.frame_time = static_cast<float>(ctx.wall_time);
    impl.frame_resolution[0] = static_cast<float>(GetScreenWidth());
    impl.frame_resolution[1] = static_cast<float>(GetScreenHeight());

    const double age = static_cast<double>(ctx.position_us - ctx.line_time_us) / 1e6;
    const double duration = static_cast<double>(ctx.line_end_us - ctx.line_time_us) / 1e6;
    const double progress = duration > 0.0 ? age / duration : 0.0;
    impl.frame_progress = static_cast<float>(progress);

    sol::table line = impl.lua.create_table();
    line["text"] = ctx.line_text;
    line["index"] = ctx.line_index;
    line["age"] = age;
    line["progress"] = progress;
    line["duration"] = duration;

    sol::table next = impl.lua.create_table();
    next["text"] = ctx.next_text;

    sol::table colors = impl.lua.create_table();
    sol::table current = impl.lua.create_table();
    current[1] = impl.current_color[0];
    current[2] = impl.current_color[1];
    current[3] = impl.current_color[2];
    current[4] = impl.current_color[3];
    sol::table next_color = impl.lua.create_table();
    next_color[1] = impl.next_color[0];
    next_color[2] = impl.next_color[1];
    next_color[3] = impl.next_color[2];
    next_color[4] = impl.next_color[3];
    colors["current"] = current;
    colors["next"] = next_color;

    sol::table lines = impl.lua.create_table();
    for (std::size_t i = 0; i < ctx.lines.size(); i++) {
        sol::table entry = impl.lua.create_table();
        entry["text"] = ctx.lines[i].text;
        entry["index"] = static_cast<int>(i);
        entry["time"] = static_cast<double>(ctx.lines[i].time_us) / 1e6;
        lines[static_cast<int>(i) + 1] = entry;
    }

    sol::table ctx_table = impl.lua.create_table();
    ctx_table["position_us"] = ctx.position_us;
    ctx_table["dt"] = ctx.dt;
    ctx_table["wall_time"] = ctx.wall_time;
    ctx_table["seeked"] = ctx.seeked;
    ctx_table["index"] = ctx.line_index;
    ctx_table["count"] = ctx.line_count;
    ctx_table["line"] = line;
    ctx_table["next"] = next;
    ctx_table["lines"] = lines;
    ctx_table["colors"] = colors;
    ctx_table["state"] = impl.state;

    if (impl.last_line_index != ctx.line_index && impl.on_lyric.valid()) {
        impl.last_line_index = ctx.line_index;
        sol::protected_function_result result = impl.on_lyric(ctx_table);
        if (!result.valid()) {
            impl.Fail(result);
            return;
        }
    }
    impl.last_line_index = ctx.line_index;

    if (impl.on_frame.valid()) {
        sol::protected_function_result result = impl.on_frame(impl.frame, ctx_table);
        if (!result.valid()) {
            impl.Fail(result);
            impl.DrawDefault(ctx);
            return;
        }
    }

    impl.Execute(0, impl.commands.Size());
}

bool PluginHost::using_default() const { return impl_->is_default; }

const PresetSetup& PluginHost::setup() const { return impl_->setup; }

uint64_t PluginHost::reload_serial() const { return impl_->reload_serial; }

}  // namespace raylyrics
