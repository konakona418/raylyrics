#include "raylib.h"
#include "rlgl.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>

#include <gio/gio.h>

#include "config/config.h"
#include "ipc/control.h"
#include "lyrics/cache.h"
#include "lyrics/encoding.h"
#include "lyrics/lrc.h"
#include "lyrics/lrclib.h"
#include "media/media.h"
#include "plugin/plugin.h"
#include "render/text.h"
#include "wayland/wayland_glue.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

double MonotonicSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void FormatPosition(int64_t position_us, char* out, size_t size) {
    if (position_us < 0) position_us = 0;
    const int64_t total_ms = position_us / 1000;
    const int64_t minutes = total_ms / 60000;
    const int64_t seconds = (total_ms / 1000) % 60;
    const int64_t millis = total_ms % 1000;
    std::snprintf(out, size, "%02lld:%02lld.%03lld", static_cast<long long>(minutes),
                  static_cast<long long>(seconds), static_cast<long long>(millis));
}

int RunMprisDump() {
    media_backend* backend = media_mpris_create();
    if (backend == nullptr) {
        std::fprintf(stderr, "raylyrics: failed to create MPRIS backend\n");
        return 1;
    }

    uint64_t last_generation = ~0ull;
    double last_print = 0.0;
    char position[32] = {0};

    while (!g_stop) {
        media_poll(backend);

        if (media_selection_dirty(backend)) {
            media_set_active(backend, nullptr);
            const int player_count = media_player_count(backend);
            std::printf("players (%d):\n", player_count);
            for (int i = 0; i < player_count; i++) {
                const media_player_info* info = media_player_at(backend, i);
                if (info == nullptr) continue;
                std::printf("  %s  [%s]  %s\n", info->name, info->playing ? "playing" : "paused",
                            info->identity != nullptr ? info->identity : "?");
            }
            std::fflush(stdout);
        }

        const media_state* state = media_get_state(backend);

        const bool track_changed = state->generation != last_generation;
        const double now = MonotonicSeconds();

        if (track_changed) {
            last_generation = state->generation;
            std::printf("\n%s:\n  Artist: %s\n  Title: %s\n  Album: %s\n  Status: %s\n",
                        (state->player && state->player[0]) ? state->player : "(no player)",
                        state->artist, state->title, state->album,
                        !state->valid ? "none" : (state->playing ? "Playing" : "Paused/Stopped"));
            last_print = 0.0;
        }

        if (state->valid && now - last_print >= 0.5) {
            FormatPosition(media_current_position_us(state), position, sizeof(position));
            std::printf("  Position: %s\n", position);
            std::fflush(stdout);
            last_print = now;
        }

        struct timespec delay = {0, 100 * 1000 * 1000};
        nanosleep(&delay, nullptr);
    }

    media_mpris_destroy(backend);
    return 0;
}

struct LyricsQuery {
    GMainLoop* loop = nullptr;
    lrclib_result result{};
    std::string synced;
    std::string plain;
    std::string track_name;
    std::string artist_name;
    std::string album_name;
    bool done = false;
};

void OnLyricsResponse(void* user_data, const lrclib_result* result) {
    auto* query = static_cast<LyricsQuery*>(user_data);

    if (result->synced_lrc != nullptr) query->synced = result->synced_lrc;
    if (result->plain_lyrics != nullptr) query->plain = result->plain_lyrics;
    if (result->track_name != nullptr) query->track_name = result->track_name;
    if (result->artist_name != nullptr) query->artist_name = result->artist_name;
    if (result->album_name != nullptr) query->album_name = result->album_name;

    query->result = *result;
    query->result.synced_lrc = query->synced.empty() ? nullptr : query->synced.c_str();
    query->result.plain_lyrics = query->plain.empty() ? nullptr : query->plain.c_str();
    query->result.track_name = query->track_name.c_str();
    query->result.artist_name = query->artist_name.c_str();
    query->result.album_name = query->album_name.c_str();

    query->done = true;
    g_main_loop_quit(query->loop);
}

size_t CountUtf8(const char* text) {
    size_t count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p) {
        if ((*p & 0xC0) != 0x80) count++;
    }
    return count;
}

// Firefox/Spotify often reports an empty artist and packs "Title • Artist".
void SplitCombinedTitle(const std::string& combined, std::string* title, std::string* artist) {
    static const char* separators[] = {" • ", " - ", " – ", " — "};
    for (const char* separator : separators) {
        const size_t position = combined.find(separator);
        if (position != std::string::npos) {
            // NOTE: `combined` may alias `*title`; build both halves first.
            std::string head = combined.substr(0, position);
            std::string tail = combined.substr(position + std::strlen(separator));
            *title = std::move(head);
            *artist = std::move(tail);
            return;
        }
    }
    *title = combined;
    artist->clear();
}

int RunLyricsQuery(const char* artist_arg, const char* title_arg, double duration) {
    std::string artist = (artist_arg != nullptr) ? artist_arg : "";
    std::string title = (title_arg != nullptr) ? title_arg : "";
    if (artist.empty()) SplitCombinedTitle(title, &title, &artist);

    std::printf("Querying LRCLIB: artist=\"%s\" title=\"%s\" duration=%.0f\n", artist.c_str(),
                title.c_str(), duration);

    lrclib_client* client = lrclib_client_new();
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    LyricsQuery query;
    query.loop = loop;

    lrclib_get(client, artist.c_str(), title.c_str(), nullptr, duration, OnLyricsResponse, &query);
    g_main_loop_run(loop);

    if (!query.done) {
        std::fprintf(stderr, "raylyrics: no response\n");
    } else {
        std::printf("HTTP %d: %s - %s (%.0fs)%s\n", query.result.status,
                    (query.result.artist_name != nullptr) ? query.result.artist_name : "?",
                    (query.result.track_name != nullptr) ? query.result.track_name : "?",
                    query.result.duration, query.result.instrumental ? " [instrumental]" : "");
    }

    lrc_doc* doc = nullptr;
    if (!query.synced.empty()) doc = lrc_parse(query.synced.data(), query.synced.size());

    int result_code = 1;
    if (doc != nullptr) {
        const size_t count = lrc_line_count(doc);
        std::printf("Parsed %zu synced lines (title=%s artist=%s)\n", count, lrc_title(doc),
                    lrc_artist(doc));
        for (size_t i = 0; i < count; i++) {
            const lrc_line* line = lrc_line_at(doc, i);
            std::printf("  [%8.3f] %s%s\n", static_cast<double>(line->time_us) / 1e6, line->text,
                        line->syllable_count > 0 ? "   (enhanced)" : "");
        }

        const int64_t last = lrc_line_at(doc, count - 1)->time_us;
        int previous_line = -2;
        size_t previous_highlight = static_cast<size_t>(-1);
        int printed = 0;
        for (int64_t t = 0; t <= last + 2000000 && printed < 60; t += 200000) {
            const int index = lrc_line_index_at(doc, t);
            if (index < 0) continue;
            const lrc_line* line = lrc_line_at(doc, static_cast<size_t>(index));
            const size_t highlight = lrc_highlight_count(doc, static_cast<size_t>(index), t);
            if (index != previous_line || highlight != previous_highlight) {
                std::printf("[%6.2fs] line %d highlight %zu/%zu: %s\n",
                            static_cast<double>(t) / 1e6, index, highlight, CountUtf8(line->text),
                            line->text);
                previous_line = index;
                previous_highlight = highlight;
                printed++;
            }
        }
        lrc_free(doc);
        result_code = 0;
    } else if (!query.plain.empty()) {
        std::printf("No synced lyrics; %zu bytes of plain lyrics available\n", query.plain.size());
    } else {
        std::printf("No lyrics found\n");
    }

    g_main_loop_unref(loop);
    lrclib_client_free(client);
    return result_code;
}

std::string ReadFileText(const std::string& path, bool* ok) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *ok = false;
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    *ok = true;
    return content;
}

std::string SanitizeFileName(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        if (c == '/' || c == '\0') c = '_';
    }
    return out;
}

lrc_doc* TryLocalLyrics(const std::string& url, std::string artist, std::string title,
                        const std::string& lyrics_root) {
    // 1. Sibling .lrc next to a local file:// URL.
    if (!url.empty() && url.rfind("file://", 0) == 0) {
        std::string path = url.substr(7);
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos) path = path.substr(0, dot) + ".lrc";
        bool ok = false;
        const std::string content = ReadFileText(path, &ok);
        if (ok) {
            const std::string utf8 = raylyrics::ToUtf8(content);
            if (lrc_doc* doc = lrc_parse(utf8.data(), utf8.size())) return doc;
        }
    }

    // 2. Configured lyrics root: "Artist - Title.lrc".
    if (artist.empty()) SplitCombinedTitle(title, &title, &artist);

    const std::string path = lyrics_root + "/" + SanitizeFileName(artist) + " - " +
                             SanitizeFileName(title) + ".lrc";
    bool ok = false;
    const std::string content = ReadFileText(path, &ok);
    if (ok) {
        const std::string utf8 = raylyrics::ToUtf8(content);
        return lrc_parse(utf8.data(), utf8.size());
    }
    return nullptr;
}

struct LyricsRuntime {
    lrclib_client* client = nullptr;
    raylyrics::LyricsCache* cache = nullptr;
    lrc_doc* doc = nullptr;
    uint64_t generation = ~0ull;
    uint64_t doc_serial = 0;
    bool fetching = false;
};

struct RemoteRequest {
    LyricsRuntime* runtime;
    uint64_t generation;
    std::string artist;
    std::string title;
    std::string album;
    double duration = 0.0;
};

void OnRemoteLyrics(void* user_data, const lrclib_result* result) {
    auto* request = static_cast<RemoteRequest*>(user_data);
    LyricsRuntime* runtime = request->runtime;
    const uint64_t generation = request->generation;
    const std::string artist = request->artist;
    const std::string title = request->title;
    const std::string album = request->album;
    const double duration = request->duration;
    delete request;

    // The track changed while this request was in flight; drop the response.
    if (generation != runtime->generation) return;

    runtime->fetching = false;
    if (result->synced_lrc != nullptr) {
        if (runtime->cache != nullptr) {
            runtime->cache->Put(artist, title, album, duration, result->synced_lrc, "lrclib");
        }
        if (runtime->doc != nullptr) lrc_free(runtime->doc);
        runtime->doc = lrc_parse(result->synced_lrc, std::strlen(result->synced_lrc));
        runtime->doc_serial++;
        std::fprintf(stderr, "raylyrics: fetched %s - %s (%zu lines)\n",
                     result->artist_name != nullptr ? result->artist_name : "?",
                     result->track_name != nullptr ? result->track_name : "?",
                     runtime->doc != nullptr ? lrc_line_count(runtime->doc) : 0);
    } else {
        std::fprintf(stderr, "raylyrics: no synced lyrics (HTTP %d)\n", result->status);
    }
}

bool IsBlankLine(const lrc_line* line) {
    if (line == nullptr || line->text == nullptr) return true;
    for (const char* p = line->text; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\r') return false;
    }
    return true;
}

int RunLrcFile(const char* path) {
    bool ok = false;
    const std::string content = ReadFileText(path, &ok);
    if (!ok) {
        std::fprintf(stderr, "raylyrics: cannot read %s\n", path);
        return 1;
    }

    const std::string utf8 = raylyrics::ToUtf8(content);
    lrc_doc* doc = lrc_parse(utf8.data(), utf8.size());
    if (doc == nullptr) {
        std::fprintf(stderr, "raylyrics: no lyrics parsed\n");
        return 1;
    }

    const size_t count = lrc_line_count(doc);
    std::printf("%zu lines\n", count);
    for (size_t i = 0; i < count; i++) {
        const lrc_line* line = lrc_line_at(doc, i);
        std::printf("  [%8.3f] %s%s\n", static_cast<double>(line->time_us) / 1e6, line->text,
                    IsBlankLine(line) ? "   (blank)" : "");
    }

    const int64_t last = lrc_line_at(doc, count - 1)->time_us;
    int previous_display = -2;
    size_t previous_highlight = static_cast<size_t>(-1);
    for (int64_t t = 0; t <= last + 2000000; t += 200000) {
        const int active = lrc_line_index_at(doc, t);
        int display = active;
        if (display >= 0 && IsBlankLine(lrc_line_at(doc, static_cast<size_t>(display)))) display = -1;
        const size_t highlight =
            (display >= 0 && t >= lrc_line_at(doc, static_cast<size_t>(display))->time_us)
                ? lrc_highlight_count(doc, static_cast<size_t>(display), t)
                : 0;
        if (display != previous_display || highlight != previous_highlight) {
            std::printf("[%6.2fs] active=%d display=%d highlight=%zu  %s\n",
                        static_cast<double>(t) / 1e6, active, display, highlight,
                        display >= 0 ? lrc_line_at(doc, static_cast<size_t>(display))->text
                                     : "(interlude)");
            previous_display = display;
            previous_highlight = highlight;
        }
    }

    lrc_free(doc);
    return 0;
}

std::string BuildSongText(const lrc_doc* doc) {
    std::string out;
    const size_t count = lrc_line_count(doc);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) out += '\n';
        out += lrc_line_at(doc, i)->text;
    }
    return out;
}

// Bridges the LRCLIB /api/search candidate list to config.on_search.
int OnSearchCandidates(void* user_data, const char* artist, const char* title, const char* album,
                       double query_duration, const lrclib_candidate* candidates, int count) {
    auto* plugin = static_cast<raylyrics::PluginHost*>(user_data);
    if (plugin == nullptr || count <= 0) return -1;

    raylyrics::PluginMetadata query;
    query.artist = artist != nullptr ? artist : "";
    query.title = title != nullptr ? title : "";
    query.album = album != nullptr ? album : "";
    query.duration_us = static_cast<int64_t>(query_duration * 1e6);

    std::vector<raylyrics::PluginSearchResult> results(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const lrclib_candidate& candidate = candidates[i];
        raylyrics::PluginSearchResult& result = results[static_cast<size_t>(i)];
        result.track_name = candidate.track_name != nullptr ? candidate.track_name : "";
        result.artist_name = candidate.artist_name != nullptr ? candidate.artist_name : "";
        result.album_name = candidate.album_name != nullptr ? candidate.album_name : "";
        result.duration = candidate.duration;
        result.has_synced = candidate.has_synced != 0;
        result.has_plain = candidate.has_plain != 0;
    }
    return plugin->SelectSearchResult(query, results);
}

int RunCacheCommand(int argc, char** argv) {
    const raylyrics::Config config = raylyrics::Config::LoadDefault();
    const long ttl_seconds = static_cast<long>(config.cache_ttl_days) * 24 * 60 * 60;
    raylyrics::LyricsCache cache(raylyrics::LyricsCache::DefaultDir(), ttl_seconds);
    const std::string action = (argc > 2) ? argv[2] : "list";

    if (action == "dir") {
        std::printf("%s\n", cache.dir().c_str());
        return 0;
    }

    if (action == "list") {
        const std::vector<raylyrics::LyricsCache::Entry> entries = cache.List();
        const long now = static_cast<long>(std::time(nullptr));
        for (const raylyrics::LyricsCache::Entry& entry : entries) {
            const long age_seconds = now - entry.mtime;
            char age[16];
            if (age_seconds >= 24 * 3600) {
                std::snprintf(age, sizeof(age), "%ldd", age_seconds / (24 * 3600));
            } else {
                std::snprintf(age, sizeof(age), "%ldh", age_seconds / 3600);
            }
            std::printf("%s  %s - %s", entry.key.c_str(), entry.artist.c_str(),
                        entry.title.c_str());
            if (!entry.album.empty()) std::printf("  [%s]", entry.album.c_str());
            std::printf("  %lds  %ldB  %s  %s\n", static_cast<long>(entry.duration + 0.5),
                        entry.size, age, entry.source.c_str());
        }
        std::printf("%zu entries in %s (ttl %ldd)\n", entries.size(), cache.dir().c_str(),
                    cache.ttl_seconds() / (24 * 3600));
        return 0;
    }

    if (action == "clear") {
        std::printf("removed %d entries from %s\n", cache.Clear(), cache.dir().c_str());
        return 0;
    }

    if (action == "prune") {
        std::printf("removed %d expired entries from %s\n", cache.Prune(), cache.dir().c_str());
        return 0;
    }

    if (action == "remove") {
        if (argc < 4) {
            std::fprintf(stderr, "usage: raylyrics cache remove <key>\n");
            return 1;
        }
        if (!cache.Remove(argv[3])) {
            std::fprintf(stderr, "raylyrics: no such entry '%s'\n", argv[3]);
            return 1;
        }
        std::printf("removed %s\n", argv[3]);
        return 0;
    }

    std::fprintf(stderr, "usage: raylyrics cache [list|clear|prune|remove <key>|dir]\n");
    return 1;
}

// "fullscreen" anchors all four edges; otherwise every direction named in the
// string is anchored (e.g. "bottom", "top-left", "top-left-right-bottom").
int ParseAnchor(const std::string& name) {
    if (name.find("full") != std::string::npos) {
        return RL_WL_ANCHOR_TOP | RL_WL_ANCHOR_BOTTOM | RL_WL_ANCHOR_LEFT | RL_WL_ANCHOR_RIGHT;
    }
    int anchor = 0;
    if (name.find("left") != std::string::npos) anchor |= RL_WL_ANCHOR_LEFT;
    if (name.find("right") != std::string::npos) anchor |= RL_WL_ANCHOR_RIGHT;
    if (name.find("top") != std::string::npos) anchor |= RL_WL_ANCHOR_TOP;
    if (name.find("bottom") != std::string::npos) anchor |= RL_WL_ANCHOR_BOTTOM;
    return anchor;
}

int RunOverlay() {
    const raylyrics::Config config = raylyrics::Config::LoadDefault();

    // The preset can declare bootstrap settings (viewport, layer, anchor, ...),
    // so evaluate it before the window exists. Neither the Lua host nor the
    // config needs GL; presets must not call f:* at load time.
    raylyrics::PluginHost plugin;
    plugin.LoadConfig(raylyrics::Config::DefaultPath());
    plugin.SetPreset(config.preset);
    const raylyrics::PresetSetup& setup = plugin.setup();

    // layer-shell centers a surface horizontally when neither left nor right is
    // anchored; anchoring both pins it to the left edge with the requested width.
    const std::string anchor_name = setup.has_anchor ? setup.anchor : config.overlay.anchor;
    int anchor = ParseAnchor(anchor_name);
    if (anchor == 0) anchor = RL_WL_ANCHOR_BOTTOM;

    const int margin_top = setup.has_margin ? setup.margin_top : config.overlay.margin_top;
    const int margin_right = setup.has_margin ? setup.margin_right : config.overlay.margin_right;
    const int margin_bottom = setup.has_margin ? setup.margin_bottom : config.overlay.margin_bottom;
    const int margin_left = setup.has_margin ? setup.margin_left : config.overlay.margin_left;
    const std::string output = setup.has_output ? setup.output : config.overlay.output;
    const int viewport_width = setup.has_viewport ? setup.viewport_width : config.overlay.width;
    const int viewport_height = setup.has_viewport ? setup.viewport_height : config.overlay.height;

    rl_wl_set_geometry(anchor, margin_top, margin_right, margin_bottom, margin_left);
    rl_wl_set_output(output.empty() ? nullptr : output.c_str());
    rl_wl_set_size(viewport_width, viewport_height);
    if (setup.has_layer) rl_wl_set_layer(setup.layer);
    if (setup.has_namespace) rl_wl_set_namespace(setup.layer_namespace.c_str());
    if (setup.has_exclusive_zone) rl_wl_set_exclusive_zone(setup.exclusive_zone);
    if (setup.has_keyboard) rl_wl_set_keyboard(setup.keyboard ? 1 : 0);

    // raylib wants positive dimensions; the platform replaces them with the
    // resolved surface size once the compositor has configured the surface.
    InitWindow(viewport_width > 0 ? viewport_width : 800,
               viewport_height > 0 ? viewport_height : 160, "raylyrics");
    if (!IsWindowReady()) {
        std::fprintf(stderr, "raylyrics: failed to initialize window\n");
        return 1;
    }

    // Wayland surfaces are composited with premultiplied alpha. Blend RGB
    // straight but accumulate alpha premultiplied so anti-aliased edges do not
    // produce halos.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                              RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);

    uint64_t last_serial = ~0ull;

    // fps/font/colors may change on hot reload; re-apply after a preset switch.
    auto apply_runtime_setup = [&]() {
        const raylyrics::PresetSetup& current = plugin.setup();
        SetTargetFPS(current.has_fps ? current.fps : config.fps);

        raylyrics::TextStyle style;
        style.families = config.font.families;
        style.size = config.font.size;
        style.line_spacing = config.font.line_spacing;
        style.letter_spacing = config.font.letter_spacing;
        if (current.has_font) {
            if (!current.font.families.empty()) style.families = current.font.families;
            style.size = current.font.size;
            style.line_spacing = current.font.line_spacing;
            style.letter_spacing = current.font.letter_spacing;
        }
        plugin.SetStyle(style);

        if (current.has_colors) {
            plugin.SetColors(current.colors_current, current.colors_next);
        } else {
            plugin.SetColors(config.colors.current, config.colors.next);
        }
        last_serial = ~0ull;  // re-prepare the text with the new style
    };
    apply_runtime_setup();
    uint64_t last_reload_serial = plugin.reload_serial();

    raylyrics::ControlServer control;
    bool hidden = false;
    int64_t lyric_offset_us = 0;
    std::string active_preset = config.preset;

    raylyrics::LyricsCache cache(raylyrics::LyricsCache::DefaultDir());
    media_backend* media = media_mpris_create();
    LyricsRuntime runtime;
    runtime.client = lrclib_client_new();
    runtime.cache = &cache;
    lrclib_set_selector(runtime.client, OnSearchCandidates, &plugin);

    double last_wall_time = MonotonicSeconds();
    int64_t last_position = -1;

    while (!g_stop && !WindowShouldClose()) {
        media_poll(media);  // also drains the GLib context, delivering LRCLIB callbacks

        if (media_selection_dirty(media)) {
            std::vector<raylyrics::PluginPlayer> players;
            const int player_count = media_player_count(media);
            players.reserve(static_cast<size_t>(player_count));
            for (int i = 0; i < player_count; i++) {
                const media_player_info* info = media_player_at(media, i);
                if (info == nullptr) continue;
                raylyrics::PluginPlayer player;
                player.name = info->name != nullptr ? info->name : "";
                player.identity = info->identity != nullptr ? info->identity : "";
                player.title = info->title != nullptr ? info->title : "";
                player.artist = info->artist != nullptr ? info->artist : "";
                player.album = info->album != nullptr ? info->album : "";
                player.playing = info->playing;
                player.position_us = info->position_us;
                player.length_us = info->length_us;
                players.push_back(std::move(player));
            }
            const std::string chosen = plugin.SelectPlayer(players);
            media_set_active(media, chosen.empty() ? nullptr : chosen.c_str());
        }

        const media_state* state = media_get_state(media);

        const std::string command = control.Poll();
        if (!command.empty()) {
            if (command == "hide") {
                hidden = true;
            } else if (command == "show") {
                hidden = false;
            } else if (command == "toggle") {
                hidden = !hidden;
            } else if (command == "quit") {
                g_stop = 1;
            } else if (command == "reload") {
                plugin.SetPreset(active_preset);
                apply_runtime_setup();
            } else if (command.rfind("preset ", 0) == 0) {
                active_preset = command.substr(7);
                plugin.SetPreset(active_preset);
                apply_runtime_setup();
            } else if (command.rfind("offset ", 0) == 0) {
                lyric_offset_us = static_cast<int64_t>(std::atof(command.substr(7).c_str()) * 1e6);
                std::fprintf(stderr, "raylyrics: lyric offset %+.3fs\n",
                             static_cast<double>(lyric_offset_us) / 1e6);
            } else {
                std::fprintf(stderr, "raylyrics: unknown command '%s'\n", command.c_str());
            }
        }

        if (state->generation != runtime.generation) {
            runtime.generation = state->generation;
            if (runtime.doc != nullptr) {
                lrc_free(runtime.doc);
                runtime.doc = nullptr;
                runtime.doc_serial++;
            }

            raylyrics::PluginMetadata meta;
            meta.artist = state->artist != nullptr ? state->artist : "";
            meta.title = state->title != nullptr ? state->title : "";
            meta.album = state->album != nullptr ? state->album : "";
            meta.duration_us = state->length_us;
            meta.player = state->player != nullptr ? state->player : "";
            plugin.NormalizeMetadata(meta);

            if (state->valid && !meta.title.empty()) {
                std::string artist = meta.artist;
                std::string title = meta.title;
                if (artist.empty()) SplitCombinedTitle(title, &title, &artist);
                const double duration_seconds = static_cast<double>(meta.duration_us) / 1e6;
                const std::string url = state->url != nullptr ? state->url : "";

                runtime.doc = TryLocalLyrics(url, artist, title, config.lyrics_root);
                if (runtime.doc != nullptr) {
                    std::fprintf(stderr, "raylyrics: local lyrics for %s (%zu lines)\n",
                                 title.c_str(), lrc_line_count(runtime.doc));
                    runtime.doc_serial++;
                } else {
                    std::string cached;
                    if (cache.Get(artist, title, meta.album, duration_seconds, &cached)) {
                        runtime.doc = lrc_parse(cached.data(), cached.size());
                        std::fprintf(stderr, "raylyrics: cached lyrics for %s (%zu lines)\n",
                                     title.c_str(),
                                     runtime.doc != nullptr ? lrc_line_count(runtime.doc) : 0);
                        runtime.doc_serial++;
                    } else {
                        runtime.fetching = true;
                        auto* request = new RemoteRequest{&runtime, state->generation};
                        request->artist = artist;
                        request->title = title;
                        request->album = meta.album;
                        request->duration = duration_seconds;
                        lrclib_get(runtime.client, artist.c_str(), title.c_str(), meta.album.c_str(),
                                   duration_seconds, OnRemoteLyrics, request);
                    }
                }
            }
        }

        if (runtime.doc_serial != last_serial) {
            last_serial = runtime.doc_serial;
            plugin.PrepareText(runtime.doc != nullptr ? BuildSongText(runtime.doc) : std::string());
        }

        const int64_t position = media_current_position_us(state) + lyric_offset_us;
        const size_t line_count = (runtime.doc != nullptr) ? lrc_line_count(runtime.doc) : 0;
        const int active = (runtime.doc != nullptr) ? lrc_line_index_at(runtime.doc, position) : -1;

        // A blank line with a timestamp marks an interlude. Respect it: while it
        // is active there is no lyric to show, and the next line must not appear
        // until its own timestamp.
        int display = active;
        if (display >= 0 && IsBlankLine(lrc_line_at(runtime.doc, static_cast<size_t>(display)))) {
            display = -1;
        }
        int next = (display >= 0) ? display + 1 : -1;
        if (next >= 0 && (static_cast<size_t>(next) >= line_count ||
                          IsBlankLine(lrc_line_at(runtime.doc, static_cast<size_t>(next))))) {
            next = -1;
        }

        const double wall_time = MonotonicSeconds();
        const double dt = wall_time - last_wall_time;
        last_wall_time = wall_time;

        raylyrics::PluginContext ctx;
        ctx.position_us = position;
        ctx.dt = dt;
        ctx.wall_time = wall_time;
        ctx.seeked = (last_position >= 0 && std::llabs(position - last_position) > 2000000);
        last_position = position;
        ctx.line_index = display;
        ctx.line_count = static_cast<int>(line_count);
        if (runtime.doc != nullptr && display >= 0) {
            const lrc_line* line = lrc_line_at(runtime.doc, static_cast<size_t>(display));
            ctx.line_text = line->text;
            ctx.line_time_us = line->time_us;
            ctx.line_end_us =
                (static_cast<size_t>(display + 1) < line_count)
                    ? lrc_line_at(runtime.doc, static_cast<size_t>(display + 1))->time_us
                    : line->time_us + 3000000;
        }
        if (runtime.doc != nullptr && next >= 0) {
            ctx.next_text = lrc_line_at(runtime.doc, static_cast<size_t>(next))->text;
        }
        if (runtime.doc != nullptr) {
            ctx.lines.reserve(line_count);
            for (size_t i = 0; i < line_count; i++) {
                const lrc_line* line = lrc_line_at(runtime.doc, i);
                ctx.lines.push_back({line->text, line->time_us});
            }
        }

        BeginDrawing();
        ClearBackground(BLANK);
        if (!hidden) plugin.RunFrame(ctx);
        EndDrawing();

        // Hot reload may have changed fps/font/colors.
        if (plugin.reload_serial() != last_reload_serial) {
            last_reload_serial = plugin.reload_serial();
            apply_runtime_setup();
        }
    }

    if (runtime.doc != nullptr) lrc_free(runtime.doc);
    lrclib_client_free(runtime.client);
    media_mpris_destroy(media);
    CloseWindow();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (argc > 1 && std::strcmp(argv[1], "--mpris") == 0) {
        return RunMprisDump();
    }

    if (argc > 1 && std::strcmp(argv[1], "cache") == 0) {
        return RunCacheCommand(argc, argv);
    }

    if (argc > 3 && std::strcmp(argv[1], "--lyrics") == 0) {
        const double duration = (argc > 4) ? std::atof(argv[4]) : 0.0;
        return RunLyricsQuery(argv[2], argv[3], duration);
    }

    if (argc > 2 && std::strcmp(argv[1], "--lrc") == 0) {
        return RunLrcFile(argv[2]);
    }

    if (argc > 2 && std::strcmp(argv[1], "ctl") == 0) {
        std::string command;
        for (int i = 2; i < argc; i++) {
            if (i > 2) command += " ";
            command += argv[i];
        }
        if (!raylyrics::SendControlCommand(command)) {
            std::fprintf(stderr, "raylyrics: no running instance\n");
            return 1;
        }
        return 0;
    }

    return RunOverlay();
}
