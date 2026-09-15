#include "media/media.h"

#include <gio/gio.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kMprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* kPlayerctld = "org.mpris.MediaPlayer2.playerctld";
constexpr const char* kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";

int64_t NowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool IsFirefox(const std::string& name) { return name.find("firefox") != std::string::npos; }

}  // namespace

struct media_backend {
    GDBusConnection* conn = nullptr;
    guint name_owner_sub = 0;
    guint props_sub = 0;
    guint seeked_sub = 0;

    std::string player;
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

    media_state state{};

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

    void Refresh() {
        state.title = title.c_str();
        state.artist = artist.c_str();
        state.album = album.c_str();
        state.track_id = track_id.c_str();
        state.url = url.c_str();
        state.player = player.c_str();
        state.valid = valid;
        state.playing = playing;
        state.position_us = position_us;
        state.position_stamp_us = position_stamp_us;
        state.length_us = length_us;
        state.rate = rate;
        state.generation = generation;
    }

    void ParseMetadata(GVariant* metadata) {
        const gchar* text = nullptr;

        if (g_variant_lookup(metadata, "xesam:title", "&s", &text)) title = text;
        if (g_variant_lookup(metadata, "xesam:album", "&s", &text)) album = text;
        if (g_variant_lookup(metadata, "xesam:url", "&s", &text)) url = text;
        if (g_variant_lookup(metadata, "mpris:trackid", "&o", &text)) track_id = text;

        gint64 length = 0;
        if (g_variant_lookup(metadata, "mpris:length", "x", &length)) length_us = length;

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
            artist = std::move(joined);
            g_variant_unref(artists);
        } else if (g_variant_lookup(metadata, "xesam:artist", "&s", &text)) {
            artist = text;
        }
    }

    void ParsePlayerDict(GVariant* dict) {
        GVariant* metadata = nullptr;
        if (g_variant_lookup(dict, "Metadata", "@a{sv}", &metadata)) {
            ParseMetadata(metadata);
            g_variant_unref(metadata);
        }

        const gchar* status = nullptr;
        if (g_variant_lookup(dict, "PlaybackStatus", "&s", &status)) {
            playing = std::strcmp(status, "Playing") == 0;
        }

        gdouble rate_value = 0.0;
        if (g_variant_lookup(dict, "Rate", "d", &rate_value)) rate = rate_value;

        gint64 position = 0;
        if (g_variant_lookup(dict, "Position", "x", &position)) {
            position_us = position;
            position_stamp_us = NowUs();
        }
    }

    bool CallGetAll() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, player.c_str(), kMprisPath, kPropsIface, "GetAll",
            g_variant_new("(s)", kPlayerIface), G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply == nullptr) {
            std::fprintf(stderr, "raylyrics: GetAll failed on %s: %s\n", player.c_str(),
                         error ? error->message : "unknown");
            g_clear_error(&error);
            valid = false;
            return false;
        }

        GVariant* dict = nullptr;
        g_variant_get(reply, "(@a{sv})", &dict);
        ParsePlayerDict(dict);
        g_variant_unref(dict);
        g_variant_unref(reply);
        valid = true;
        return true;
    }

    void CallGetPosition() {
        if (player.empty()) return;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn, player.c_str(), kMprisPath, kPropsIface, "Get",
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
                position_us = g_variant_get_int64(inner);
                position_stamp_us = NowUs();
            }
            g_variant_unref(inner);
            g_variant_unref(wrapper);
        }
        g_variant_unref(reply);
    }

    void DetachPlayer() {
        if (props_sub != 0) {
            g_dbus_connection_signal_unsubscribe(conn, props_sub);
            props_sub = 0;
        }
        if (seeked_sub != 0) {
            g_dbus_connection_signal_unsubscribe(conn, seeked_sub);
            seeked_sub = 0;
        }
        player.clear();
        title.clear();
        artist.clear();
        album.clear();
        track_id.clear();
        url.clear();
        valid = false;
        playing = false;
        position_us = 0;
        position_stamp_us = 0;
        length_us = 0;
        rate = 1.0;
        Refresh();
    }

    void AttachPlayer(const std::string& name) {
        DetachPlayer();
        player = name;

        props_sub = g_dbus_connection_signal_subscribe(
            conn, player.c_str(), kPropsIface, "PropertiesChanged", kMprisPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant* params, gpointer user_data) {
                static_cast<media_backend*>(user_data)->OnPropertiesChanged(params);
            },
            this, nullptr);

        seeked_sub = g_dbus_connection_signal_subscribe(
            conn, player.c_str(), kPlayerIface, "Seeked", kMprisPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant* params, gpointer user_data) {
                static_cast<media_backend*>(user_data)->OnSeeked(params);
            },
            this, nullptr);

        CallGetAll();
        CallGetPosition();
        Refresh();

        std::fprintf(stderr, "raylyrics: attached to %s\n", player.c_str());
    }

    void SelectPlayer() {
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

        std::string firefox;
        std::string first;
        GVariantIter iter;
        g_variant_iter_init(&iter, names);
        const gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "&s", &name)) {
            std::string candidate(name);
            if (candidate.rfind(kMprisPrefix, 0) != 0) continue;
            if (candidate == kPlayerctld) continue;
            if (first.empty()) first = candidate;
            if (IsFirefox(candidate)) {
                firefox = candidate;
                break;
            }
        }

        g_variant_unref(names);
        g_variant_unref(reply);

        const std::string chosen = !firefox.empty() ? firefox : first;
        if (!chosen.empty() && chosen != player) AttachPlayer(chosen);
    }

    void OnPropertiesChanged(GVariant* params) {
        const gchar* interface = nullptr;
        GVariant* changed = nullptr;
        GVariant* invalidated = nullptr;
        g_variant_get(params, "(&s@a{sv}@as)", &interface, &changed, &invalidated);

        if (interface != nullptr && std::strcmp(interface, kPlayerIface) == 0) {
            const bool metadata_changed =
                g_variant_lookup_value(changed, "Metadata", nullptr) != nullptr;
            const bool status_changed =
                g_variant_lookup_value(changed, "PlaybackStatus", nullptr) != nullptr;

            const std::string previous_track = TrackSignature();
            ParsePlayerDict(changed);

            if (metadata_changed && TrackSignature() != previous_track) {
                generation++;
                position_us = 0;
                position_stamp_us = NowUs();
            }
            if (metadata_changed || status_changed) CallGetPosition();

            Refresh();
        }

        g_variant_unref(changed);
        g_variant_unref(invalidated);
    }

    void OnSeeked(GVariant* params) {
        gint64 position = 0;
        g_variant_get(params, "(x)", &position);
        position_us = position;
        position_stamp_us = NowUs();
        Refresh();
    }

    void OnNameOwnerChanged(GVariant* params) {
        const gchar* name = nullptr;
        const gchar* old_owner = nullptr;
        const gchar* new_owner = nullptr;
        g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);

        const bool appeared = new_owner != nullptr && new_owner[0] != '\0';
        const bool vanished = !appeared;

        if (player == name) {
            if (vanished) {
                std::fprintf(stderr, "raylyrics: player %s vanished\n", player.c_str());
                DetachPlayer();
                SelectPlayer();
            } else {
                AttachPlayer(player);
            }
        } else if (appeared) {
            SelectPlayer();
        }
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

    backend->SelectPlayer();
    backend->Refresh();
    return backend;
}

extern "C" void media_mpris_destroy(media_backend* backend) {
    if (backend == nullptr) return;

    if (backend->name_owner_sub != 0) {
        g_dbus_connection_signal_unsubscribe(backend->conn, backend->name_owner_sub);
    }
    if (backend->props_sub != 0) {
        g_dbus_connection_signal_unsubscribe(backend->conn, backend->props_sub);
    }
    if (backend->seeked_sub != 0) {
        g_dbus_connection_signal_unsubscribe(backend->conn, backend->seeked_sub);
    }
    if (backend->conn != nullptr) g_object_unref(backend->conn);

    delete backend;
}

extern "C" void media_poll(media_backend* backend) {
    if (backend == nullptr) return;

    while (g_main_context_iteration(nullptr, FALSE)) {
    }

    if (backend->player.empty()) backend->SelectPlayer();
    backend->Refresh();
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
