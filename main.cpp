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

lrc_doc* TryLocalLyrics(const media_state* state, const std::string& lyrics_root) {
    // 1. Sibling .lrc next to a local file:// URL.
    if (state->url != nullptr && std::strncmp(state->url, "file://", 7) == 0) {
        std::string path = state->url + 7;
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
    std::string artist = state->artist != nullptr ? state->artist : "";
    std::string title = state->title != nullptr ? state->title : "";
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
    lrc_doc* doc = nullptr;
    uint64_t generation = ~0ull;
    uint64_t doc_serial = 0;
    bool fetching = false;
};

struct RemoteRequest {
    LyricsRuntime* runtime;
    uint64_t generation;
};

void OnRemoteLyrics(void* user_data, const lrclib_result* result) {
    auto* request = static_cast<RemoteRequest*>(user_data);
    LyricsRuntime* runtime = request->runtime;
    const uint64_t generation = request->generation;
    delete request;

    // The track changed while this request was in flight; drop the response.
    if (generation != runtime->generation) return;

    runtime->fetching = false;
    if (result->synced_lrc != nullptr) {
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

int RunOverlay() {
    const raylyrics::Config config = raylyrics::Config::LoadDefault();

    // layer-shell centers a surface horizontally when neither left nor right is
    // anchored; anchoring both pins it to the left edge with the requested width.
    const std::string& anchor_name = config.overlay.anchor;
    int anchor = 0;
    if (anchor_name.find("left") != std::string::npos) anchor |= RL_WL_ANCHOR_LEFT;
    if (anchor_name.find("right") != std::string::npos) anchor |= RL_WL_ANCHOR_RIGHT;
    if (anchor_name.find("top") != std::string::npos) anchor |= RL_WL_ANCHOR_TOP;
    if (anchor_name.find("bottom") != std::string::npos) anchor |= RL_WL_ANCHOR_BOTTOM;
    if (anchor == 0) anchor = RL_WL_ANCHOR_BOTTOM;
    rl_wl_set_geometry(anchor, config.overlay.margin_top, config.overlay.margin_right,
                       config.overlay.margin_bottom, config.overlay.margin_left);
    rl_wl_set_output(config.overlay.output.empty() ? nullptr : config.overlay.output.c_str());

    InitWindow(config.overlay.width, config.overlay.height, "raylyrics");
    if (!IsWindowReady()) {
        std::fprintf(stderr, "raylyrics: failed to initialize window\n");
        return 1;
    }

    SetTargetFPS(config.fps);

    // Wayland surfaces are composited with premultiplied alpha. Blend RGB
    // straight but accumulate alpha premultiplied so anti-aliased edges do not
    // produce halos.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                              RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);

    raylyrics::TextStyle style;
    style.families = config.font.families;
    style.size = config.font.size;
    style.line_spacing = config.font.line_spacing;
    style.letter_spacing = config.font.letter_spacing;

    raylyrics::PluginHost plugin;
    plugin.SetStyle(style);
    plugin.SetColors(config.colors.current, config.colors.next);
    plugin.LoadConfig(raylyrics::Config::DefaultPath());
    plugin.SetPreset(config.preset);

    raylyrics::ControlServer control;
    bool hidden = false;
    int64_t lyric_offset_us = 0;
    std::string active_preset = config.preset;

    media_backend* media = media_mpris_create();
    LyricsRuntime runtime;
    runtime.client = lrclib_client_new();

    uint64_t last_serial = ~0ull;
    double last_wall_time = MonotonicSeconds();
    int64_t last_position = -1;

    while (!g_stop && !WindowShouldClose()) {
        media_poll(media);  // also drains the GLib context, delivering LRCLIB callbacks
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
            } else if (command.rfind("preset ", 0) == 0) {
                active_preset = command.substr(7);
                plugin.SetPreset(active_preset);
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

            if (state->valid && state->title != nullptr && state->title[0] != '\0') {
                runtime.doc = TryLocalLyrics(state, config.lyrics_root);
                if (runtime.doc != nullptr) {
                    std::fprintf(stderr, "raylyrics: local lyrics for %s (%zu lines)\n",
                                 state->title, lrc_line_count(runtime.doc));
                    runtime.doc_serial++;
                } else {
                    std::string artist = state->artist != nullptr ? state->artist : "";
                    std::string title = state->title != nullptr ? state->title : "";
                    if (artist.empty()) SplitCombinedTitle(title, &title, &artist);
                    runtime.fetching = true;
                    auto* request = new RemoteRequest{&runtime, state->generation};
                    lrclib_get(runtime.client, artist.c_str(), title.c_str(),
                               state->album, static_cast<double>(state->length_us) / 1e6,
                               OnRemoteLyrics, request);
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
