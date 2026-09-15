#ifndef RAYLYRICS_PLUGIN_EFFECTS_H
#define RAYLYRICS_PLUGIN_EFFECTS_H

#include <string>
#include <unordered_map>

#include "raylib.h"

namespace raylyrics {

// Owns the full-screen post-process shaders used by `f:layer`. Built-in effects
// are compiled on first use; custom ones are registered from Lua via
// `f:shader(name, fragment_source)`.
class EffectManager {
public:
    ~EffectManager();

    EffectManager(const EffectManager&) = delete;
    EffectManager& operator=(const EffectManager&) = delete;

    EffectManager() = default;

    // Returns false if the fragment shader failed to compile.
    bool Register(const std::string& name, const std::string& fragment_source);

    // Returns nullptr for an unknown name.
    Shader* Get(const std::string& name);

    // Unload custom shaders (called on preset reload).
    void ClearCustom();

private:
    std::unordered_map<std::string, Shader> shaders_;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_PLUGIN_EFFECTS_H
