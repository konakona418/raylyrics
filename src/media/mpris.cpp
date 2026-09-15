#include "media/media.h"

#include <gio/gio.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kMprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* kPlayerctld = "org.mpris.MediaPlayer2.playerctld";
constexpr const char* kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kRootIface = "org.mpris.MediaPlayer2";
constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";

int64_t NowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool IsFirefox(const std::string& name) { return name.find("firefox") != std::string::npos; }

bool HasKey(GVariant* dict, const char* key) {
    GVariant* value = g_variant_lookup_value(dict, key, nullptr);
    if (value == nullptr) return false;
    g_variant_unref(value);
    return true;
}

}  // namespace

struct media_backend;

struct media_player {
    media_backend* backend = nullptr;
    std::string name;
    std::string identity;
    std::string title;
    std::string artist;
    std::string album;
    std::string track_id;
    std::string url;

    bool valid = false;
    bool playing = false;
    int64_t position_us = 0;
    int64_t position_stamp_us = 0;
    int64_t length_us = 0;
    double rate = 1.0;
    uint64_t generation = 0;
    int64_t last_change_us = 0;
    guint props_sub = 0;
    guint seeked_sub = 0;

    media_player_info info{};

    // Firefox keeps a fixed mpris:trackid across tracks, so track changes must
    // be detected from the metadata itself.
    std::string TrackSignature() const {
        std::string signature = title;
        signature += '\x1f';
        signature += artist;
        signature += '\x1f';
        signature += album;
        signature += '\x1f';
        signature += std::to_string(length_us);
        return signature;
    }

    void RefreshInfo() {
        info.name = name.c_str();
        info.identity = identity.c_str();
        info.title = title.c_str();
        info.artist = artist.c_str();
        info.album = album.c_str();
        info.valid = valid;
        info.playing = playing;
        info.position_us = position_us;
        info.length_us = length_us;
    }
};

struct media_backend {
    GDBusConnection* conn = nullptr;
    guint name_owner_sub = 0;
    std::vector<media_player*> players;
    std::string active;
    bool dirty = true;

    media_state state{};
    uint64_t state_generation = 0;
    std::string state_track_key;

    media_player* Find(const std::string& name) const {
        for (media_player* player : players) {
            if (player->name == name) return player;
        }
        return nullptr;
    }

    media_player* Active() const {
        if (active.empty()) return nullptr;
        return Find(active);
    }

    void ParseMetadata(GVariant* metadata, media_player* player) {
        const gchar* text = nullptr;

        if (g_variant_lookup(metadata, "xesam:title", "&s", &text)) player->title = text;
        if (g_variant_lookup(metadata, "xesam:album", "&s", &text)) player->album = text;
        if (g_variant_lookup(metadata, "xesam:url", "&s", &text)) player->url = text;
        if (g_variant_lookup(metadata, "mpris:trackid", "&o", &text)) player->track_id = text;

        gint64 length = 0;
        if (g_variant_lookup(metadata, "mpris:length", "x", &length)) player->length_us = length;

        GVariant* artists = nullptr;
        if (g_variant_lookup(metadata, "xesam:artist", "@as", &artists)) {
            std::string joined;
            GVariantIter iter;
            g_variant_iter_init(&iter, artists);
            const gchar* name = nullptr;
            while (g_variant_iter_next(&iter, "&s", &name)) {
                if (!joined.empty()) joined += ", ";
                joined += name;
            }
            player->artist = std::move(joined);
            g_variant_unref(artists);
        } else if (g_variant_lookup(metadata, "xesam:artist", "&s", &text)) {
            player->artist = text;
        }
    }

    void ParsePlayerDict(GVariant* dict, media_player* player) {
        GVariant* metadata = nullptr;
        if (g_variant_lookup(dict, "Metadata", "@a{sv}", &metadata)) {
            ParseMetadata(metadata, player);
            g_variant_unref(metadata);
        }

        const gchar* status = nullptr;
        if (g_variant_lookup(dict, "PlaybackStatus", "&s", &status)) {
            player->playing = std::strcmp(status, "Playing") == 0;
        }

        gdouble rate_value = 0.0;
        if (g_variant_lookup(dict, "Rate", "d", &rate_value)) player->rate = rate_value;

        gint64 position = 0;
        if (g_variant_lookup(dict, "Position", "x", &position)) {
            player->position_us = position;
            player->position_stamp_us = NowUs();
        }
    }

    bool CallGetAll(media_player* player) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, player->name.c_str(), kMprisPath, kPropsIface, "GetAll",
            g_variant_new("(s)", kPlayerIface), G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply == nullptr) {
            std::fprintf(stderr, "raylyrics: GetAll failed on %s: %s\n", player->name.c_str(),
                         error ? error->message : "unknown");
            g_clear_error(&error);
            player->valid = false;
            return false;
        }

        GVariant* dict = nullptr;
        g_variant_get(reply, "(@a{sv})", &dict);
        ParsePlayerDict(dict, player);
        g_variant_unref(dict);
        g_variant_unref(reply);
        player->valid = true;
        return true;
    }

    void CallGetIdentity(media_player* player) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, player->name.c_str(), kMprisPath, kPropsIface, "Get",
            g_variant_new("(ss)", kRootIface, "Identity"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply == nullptr) {
            g_clear_error(&error);
            return;
        }

        GVariant* wrapper = nullptr;
        g_variant_get(reply, "(@v)", &wrapper);
        if (wrapper != nullptr) {
            GVariant* inner = g_variant_get_variant(wrapper);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) {
                player->identity = g_variant_get_string(inner, nullptr);
            }
            g_variant_unref(inner);
            g_variant_unref(wrapper);
        }
        g_variant_unref(reply);
    }

    void CallGetPosition(media_player* player) {
        if (player == nullptr) return;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, player->name.c_str(), kMprisPath, kPropsIface, "Get",
            g_variant_new("(ss)", kPlayerIface, "Position"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply == nullptr) {
            g_clear_error(&error);
            return;
        }

        GVariant* wrapper = nullptr;
        g_variant_get(reply, "(@v)", &wrapper);
        if (wrapper != nullptr) {
            GVariant* inner = g_variant_get_variant(wrapper);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_INT64)) {
                player->position_us = g_variant_get_int64(inner);
                player->position_stamp_us = NowUs();
            }
            g_variant_unref(inner);
            g_variant_unref(wrapper);
        }
        g_variant_unref(reply);
    }

    void AddPlayer(const std::string& name) {
        if (Find(name) != nullptr) return;

        auto* player = new media_player();
        player->backend = this;
        player->name = name;
        player->last_change_us = NowUs();

        player->props_sub = g_dbus_connection_signal_subscribe(
            conn, name.c_str(), kPropsIface, "PropertiesChanged", kMprisPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant* params, gpointer user_data) {
                auto* self = static_cast<media_player*>(user_data);
                self->backend->OnPropertiesChanged(self, params);
            },
            player, nullptr);

        player->seeked_sub = g_dbus_connection_signal_subscribe(
            conn, name.c_str(), kPlayerIface, "Seeked", kMprisPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant* params, gpointer user_data) {
                auto* self = static_cast<media_player*>(user_data);
                self->backend->OnSeeked(self, params);
            },
            player, nullptr);

        players.push_back(player);
        CallGetAll(player);
        CallGetIdentity(player);
        player->RefreshInfo();
        dirty = true;
        std::fprintf(stderr, "raylyrics: player appeared %s (%s)\n", name.c_str(),
                     player->identity.empty() ? "?" : player->identity.c_str());
    }

    void RemovePlayer(const std::string& name) {
        for (size_t i = 0; i < players.size(); i++) {
            if (players[i]->name != name) continue;
            media_player* player = players[i];
            if (player->props_sub != 0) {
                g_dbus_connection_signal_unsubscribe(conn, player->props_sub);
            }
            if (player->seeked_sub != 0) {
                g_dbus_connection_signal_unsubscribe(conn, player->seeked_sub);
            }
            players.erase(players.begin() + static_cast<long>(i));
            delete player;
            break;
        }
        if (active == name) active.clear();
        dirty = true;
        std::fprintf(stderr, "raylyrics: player vanished %s\n", name.c_str());
        RefreshState();
    }

    void OnPropertiesChanged(media_player* player, GVariant* params) {
        const gchar* interface = nullptr;
        GVariant* changed = nullptr;
        GVariant* invalidated = nullptr;
        g_variant_get(params, "(&s@a{sv}@as)", &interface, &changed, &invalidated);

        if (interface != nullptr && std::strcmp(interface, kPlayerIface) == 0) {
            const bool metadata_changed = HasKey(changed, "Metadata");
            const bool status_changed = HasKey(changed, "PlaybackStatus");

            const std::string previous_track = player->TrackSignature();
            ParsePlayerDict(changed, player);

            if (metadata_changed && player->TrackSignature() != previous_track) {
                player->generation++;
                player->position_us = 0;
                player->position_stamp_us = NowUs();
            }
            if ((metadata_changed || status_changed) && player->name == active) {
                CallGetPosition(player);
            }

            player->last_change_us = NowUs();
            player->RefreshInfo();
            if (metadata_changed || status_changed) dirty = true;
            RefreshState();
        }

        g_variant_unref(changed);
        g_variant_unref(invalidated);
    }

    void OnSeeked(media_player* player, GVariant* params) {
        gint64 position = 0;
        g_variant_get(params, "(x)", &position);
        player->position_us = position;
        player->position_stamp_us = NowUs();
        player->RefreshInfo();
        if (player->name == active) RefreshState();
    }

    void OnNameOwnerChanged(GVariant* params) {
        const gchar* name = nullptr;
        const gchar* old_owner = nullptr;
        const gchar* new_owner = nullptr;
        g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);

        if (name == nullptr || std::strncmp(name, kMprisPrefix, std::strlen(kMprisPrefix)) != 0) {
            return;
        }
        if (std::strcmp(name, kPlayerctld) == 0) return;

        const bool appeared = new_owner != nullptr && new_owner[0] != '\0';
        if (appeared) {
            AddPlayer(name);
        } else {
            RemovePlayer(name);
        }
    }

    void EnumeratePlayers() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
            &error);
        if (reply == nullptr) {
            g_clear_error(&error);
            return;
        }

        GVariant* names = nullptr;
        g_variant_get(reply, "(@as)", &names);

        GVariantIter iter;
        g_variant_iter_init(&iter, names);
        const gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "&s", &name)) {
            std::string candidate(name);
            if (candidate.rfind(kMprisPrefix, 0) != 0) continue;
            if (candidate == kPlayerctld) continue;
            AddPlayer(candidate);
        }

        g_variant_unref(names);
        g_variant_unref(reply);
    }

    // Built-in fallback policy: keep a playing active player, else prefer a
    // playing player (Firefox first), else keep the active player, else any
    // Firefox, else the first player.
    std::string SelectDefault() const {
        media_player* current = Active();
        if (current != nullptr && current->playing) return current->name;

        media_player* playing_firefox = nullptr;
        media_player* playing_any = nullptr;
        for (media_player* player : players) {
            if (!player->playing) continue;
            if (IsFirefox(player->name)) {
                playing_firefox = player;
                break;
            }
            if (playing_any == nullptr) playing_any = player;
        }
        if (playing_firefox != nullptr) return playing_firefox->name;
        if (playing_any != nullptr) return playing_any->name;
        if (current != nullptr) return current->name;

        for (media_player* player : players) {
            if (IsFirefox(player->name)) return player->name;
        }
        return players.empty() ? std::string() : players.front()->name;
    }

    void RefreshState() {
        media_player* player = Active();

        std::string key;
        if (player != nullptr) {
            key = player->name;
            key += '\x1e';
            key += player->TrackSignature();
        }
        if (key != state_track_key) {
            state_track_key = key;
            state_generation++;
        }

        state.title = player != nullptr ? player->title.c_str() : "";
        state.artist = player != nullptr ? player->artist.c_str() : "";
        state.album = player != nullptr ? player->album.c_str() : "";
        state.track_id = player != nullptr ? player->track_id.c_str() : "";
        state.url = player != nullptr ? player->url.c_str() : "";
        state.player = active.c_str();
        state.valid = player != nullptr && player->valid;
        state.playing = player != nullptr && player->playing;
        state.position_us = player != nullptr ? player->position_us : 0;
        state.position_stamp_us = player != nullptr ? player->position_stamp_us : 0;
        state.length_us = player != nullptr ? player->length_us : 0;
        state.rate = player != nullptr ? player->rate : 1.0;
        state.generation = state_generation;
    }
};

extern "C" media_backend* media_mpris_create(void) {
    auto* backend = new media_backend();

    GError* error = nullptr;
    backend->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (backend->conn == nullptr) {
        std::fprintf(stderr, "raylyrics: failed to connect to session bus: %s\n",
                     error ? error->message : "unknown");
        g_clear_error(&error);
        delete backend;
        return nullptr;
    }

    backend->name_owner_sub = g_dbus_connection_signal_subscribe(
        backend->conn, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", kMprisPrefix, G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
            GVariant* params, gpointer user_data) {
            static_cast<media_backend*>(user_data)->OnNameOwnerChanged(params);
        },
        backend, nullptr);

    backend->EnumeratePlayers();
    backend->RefreshState();
    return backend;
}

extern "C" void media_mpris_destroy(media_backend* backend) {
    if (backend == nullptr) return;

    if (backend->name_owner_sub != 0) {
        g_dbus_connection_signal_unsubscribe(backend->conn, backend->name_owner_sub);
    }
    for (media_player* player : backend->players) {
        if (player->props_sub != 0) {
            g_dbus_connection_signal_unsubscribe(backend->conn, player->props_sub);
        }
        if (player->seeked_sub != 0) {
            g_dbus_connection_signal_unsubscribe(backend->conn, player->seeked_sub);
        }
        delete player;
    }
    backend->players.clear();
    if (backend->conn != nullptr) g_object_unref(backend->conn);

    delete backend;
}

extern "C" void media_poll(media_backend* backend) {
    if (backend == nullptr) return;

    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    backend->RefreshState();
}

extern "C" const media_state* media_get_state(const media_backend* backend) {
    return backend ? &backend->state : nullptr;
}

extern "C" int64_t media_current_position_us(const media_state* state) {
    if (state == nullptr || !state->valid) return 0;
    if (!state->playing) return state->position_us;

    const int64_t now = NowUs();
    int64_t delta = now - state->position_stamp_us;
    if (delta < 0) delta = 0;

    int64_t position = state->position_us + static_cast<int64_t>(static_cast<double>(delta) * state->rate);
    if (state->length_us > 0 && position > state->length_us) position = state->length_us;
    if (position < 0) position = 0;
    return position;
}

extern "C" int media_player_count(const media_backend* backend) {
    return backend ? static_cast<int>(backend->players.size()) : 0;
}

extern "C" const media_player_info* media_player_at(const media_backend* backend, int index) {
    if (backend == nullptr || index < 0 || index >= static_cast<int>(backend->players.size())) {
        return nullptr;
    }
    return &backend->players[static_cast<size_t>(index)]->info;
}

extern "C" bool media_selection_dirty(const media_backend* backend) {
    return backend != nullptr && backend->dirty;
}

extern "C" void media_set_active(media_backend* backend, const char* name) {
    if (backend == nullptr) return;

    std::string chosen = (name != nullptr && name[0] != '\0') ? name : "";
    if (!chosen.empty() && backend->Find(chosen) == nullptr) chosen.clear();
    if (chosen.empty()) chosen = backend->SelectDefault();

    const bool switched = chosen != backend->active;
    backend->active = chosen;
    backend->dirty = false;
    if (switched) {
        backend->CallGetPosition(backend->Active());
        std::fprintf(stderr, "raylyrics: active player %s\n",
                     backend->active.empty() ? "(none)" : backend->active.c_str());
    }
    backend->RefreshState();
}

extern "C" const char* media_active_player(const media_backend* backend) {
    return backend ? backend->active.c_str() : "";
}
