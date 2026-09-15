#include "config/config.h"

#include <sol/sol.hpp>

#include <cstdlib>
#include <string>
#include <utility>

namespace raylyrics {

namespace {

std::string ExpandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (home == nullptr) return path;
    return std::string(home) + path.substr(1);
}

void ReadColor(const sol::table& table, const char* key, float out[4]) {
    sol::object value = table[key];
    if (!value.is<sol::table>()) return;
    sol::table color = value.as<sol::table>();
    for (int i = 0; i < 4; i++) {
        sol::object component = color[i + 1];
        if (component.is<float>() || component.is<double>() || component.is<int>()) {
            out[i] = component.as<float>();
        }
    }
}

}  // namespace

std::string Config::DefaultPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/raylyrics/config.lua";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.config/raylyrics/config.lua";
}

bool Config::Load(const std::string& path) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

    sol::protected_function_result result = lua.safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid()) return false;

    sol::object root_object = lua["config"];
    if (!root_object.is<sol::table>()) return false;
    sol::table root = root_object.as<sol::table>();

    sol::object overlay_object = root["overlay"];
    if (overlay_object.is<sol::table>()) {
        sol::table table = overlay_object.as<sol::table>();
        overlay.anchor = table["anchor"].get_or(overlay.anchor);
        overlay.output = table["output"].get_or(overlay.output);
        overlay.margin_top = table["margin_top"].get_or(overlay.margin_top);
        overlay.margin_right = table["margin_right"].get_or(overlay.margin_right);
        overlay.margin_bottom = table["margin_bottom"].get_or(overlay.margin_bottom);
        overlay.margin_left = table["margin_left"].get_or(overlay.margin_left);
        overlay.width = table["width"].get_or(overlay.width);
        overlay.height = table["height"].get_or(overlay.height);
    }

    fps = root["fps"].get_or(fps);

    sol::object font_object = root["font"];
    if (font_object.is<sol::table>()) {
        sol::table table = font_object.as<sol::table>();
        font.size = table["size"].get_or(font.size);
        font.line_spacing = table["line_spacing"].get_or(font.line_spacing);
        font.letter_spacing = table["letter_spacing"].get_or(font.letter_spacing);

        sol::object families_object = table["families"];
        if (families_object.is<sol::table>()) {
            sol::table families = families_object.as<sol::table>();
            std::vector<std::string> parsed;
            for (std::size_t i = 1; i <= families.size(); i++) {
                sol::object entry = families[i];
                if (entry.is<std::string>()) parsed.push_back(entry.as<std::string>());
            }
            if (!parsed.empty()) font.families = std::move(parsed);
        }
    }

    sol::object colors_object = root["colors"];
    if (colors_object.is<sol::table>()) {
        sol::table table = colors_object.as<sol::table>();
        ReadColor(table, "current", colors.current);
        ReadColor(table, "next", colors.next);
    }

    sol::object lyrics_object = root["lyrics"];
    if (lyrics_object.is<sol::table>()) {
        sol::table table = lyrics_object.as<sol::table>();
        const std::string root_path = table["root"].get_or(std::string());
        if (!root_path.empty()) lyrics_root = ExpandHome(root_path);
        cache_ttl_days = table["cache_ttl_days"].get_or(cache_ttl_days);
    }

    preset = root["preset"].get_or(preset);
    return true;
}

Config Config::LoadDefault() {
    Config config;
    config.Load(DefaultPath());
    if (config.lyrics_root.empty()) {
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const std::string base = (xdg != nullptr && xdg[0] != '\0')
                                     ? std::string(xdg)
                                     : std::string(std::getenv("HOME") != nullptr ? std::getenv("HOME")
                                                                                  : ".") +
                                           "/.local/share";
        config.lyrics_root = base + "/raylyrics/lyrics";
    }
    return config;
}

}  // namespace raylyrics
