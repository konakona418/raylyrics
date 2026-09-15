#include "plugin/effects.h"

#include <cstdio>
#include <string>

namespace raylyrics {

namespace {

// All post shaders follow raylib's convention: the previous pass is bound to
// `texture0` and sampled with `fragTexCoord`. `u_resolution`, `u_time` and
// `u_line_progress` are injected by the host.
const char* kCommonPreamble =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_time;\n"
    "uniform float u_line_progress;\n"
    "out vec4 finalColor;\n";

const char* kBloom =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_time;\n"
    "uniform float u_line_progress;\n"
    "uniform float u_amount;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec2 texel = 1.0 / u_resolution;\n"
    "  vec4 sum = vec4(0.0);\n"
    "  for (int x = -3; x <= 3; x++)\n"
    "    for (int y = -3; y <= 3; y++)\n"
    "      sum += texture(texture0, fragTexCoord + vec2(float(x), float(y)) * texel * 2.0);\n"
    "  sum /= 49.0;\n"
    "  vec4 base = texture(texture0, fragTexCoord);\n"
    "  vec3 glow = max(sum.rgb - 0.15, 0.0);\n"
    "  finalColor = vec4(base.rgb + glow * u_amount, base.a);\n"
    "}\n";

const char* kChroma =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_time;\n"
    "uniform float u_line_progress;\n"
    "uniform float u_amount;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec2 dir = (fragTexCoord - 0.5) * u_amount;\n"
    "  vec4 base = texture(texture0, fragTexCoord);\n"
    "  float r = texture(texture0, fragTexCoord + dir).r;\n"
    "  float b = texture(texture0, fragTexCoord - dir).b;\n"
    "  finalColor = vec4(r, base.g, b, base.a);\n"
    "}\n";

const char* kBlur =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_time;\n"
    "uniform float u_line_progress;\n"
    "uniform float u_amount;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec2 texel = (1.0 / u_resolution) * u_amount;\n"
    "  vec4 sum = vec4(0.0);\n"
    "  for (int x = -4; x <= 4; x++)\n"
    "    for (int y = -4; y <= 4; y++)\n"
    "      sum += texture(texture0, fragTexCoord + vec2(float(x), float(y)) * texel);\n"
    "  finalColor = sum / 81.0;\n"
    "}\n";

const char* kScanline =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_time;\n"
    "uniform float u_line_progress;\n"
    "uniform float u_amount;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec4 base = texture(texture0, fragTexCoord);\n"
    "  float scan = 1.0 - u_amount * 0.5 *\n"
    "    (0.5 + 0.5 * sin(fragTexCoord.y * u_resolution.y * 1.5 + u_time * 6.0));\n"
    "  finalColor = vec4(base.rgb * scan, base.a);\n"
    "}\n";

struct Builtin {
    const char* name;
    const char* source;
};

const Builtin kBuiltins[] = {
    {"bloom", kBloom},
    {"chroma", kChroma},
    {"blur", kBlur},
    {"scanline", kScanline},
};

const char* FindBuiltin(const std::string& name) {
    for (const Builtin& builtin : kBuiltins) {
        if (name == builtin.name) return builtin.source;
    }
    return nullptr;
}

}  // namespace

EffectManager::~EffectManager() {
    for (auto& [name, shader] : shaders_) {
        (void)name;
        UnloadShader(shader);
    }
}

bool EffectManager::Register(const std::string& name, const std::string& fragment_source) {
    auto it = shaders_.find(name);
    if (it != shaders_.end()) {
        UnloadShader(it->second);
        shaders_.erase(it);
    }
    Shader shader = LoadShaderFromMemory(nullptr, fragment_source.c_str());
    if (!IsShaderValid(shader)) {
        std::fprintf(stderr, "raylyrics: effect '%s' failed to compile\n", name.c_str());
        return false;
    }
    shaders_.emplace(name, shader);
    return true;
}

Shader* EffectManager::Get(const std::string& name) {
    auto it = shaders_.find(name);
    if (it != shaders_.end()) return &it->second;

    const char* source = FindBuiltin(name);
    if (source == nullptr) return nullptr;

    Shader shader = LoadShaderFromMemory(nullptr, source);
    if (!IsShaderValid(shader)) {
        std::fprintf(stderr, "raylyrics: builtin effect '%s' failed to compile\n", name.c_str());
        return nullptr;
    }
    return &shaders_.emplace(name, shader).first->second;
}

void EffectManager::ClearCustom() {
    for (auto& [name, shader] : shaders_) {
        (void)name;
        UnloadShader(shader);
    }
    shaders_.clear();
}

}  // namespace raylyrics
