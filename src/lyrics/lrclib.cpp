#include "lyrics/lrclib.h"

#include <cjson/cJSON.h>
#include <libsoup/soup.h>

#include <cmath>
#include <cstring>
#include <string>

struct lrclib_client {
    SoupSession* session;
};

namespace {

constexpr const char* kUserAgent = "raylyrics/0.1 (https://github.com/konakona418/slug-raylib)";

std::string Escape(const char* value) {
    if (value == nullptr || value[0] == '\0') return {};
    char* escaped = g_uri_escape_string(value, nullptr, TRUE);
    std::string out = (escaped != nullptr) ? escaped : "";
    g_free(escaped);
    return out;
}

struct RequestContext {
    SoupMessage* message = nullptr;
    lrclib_callback callback = nullptr;
    void* user_data = nullptr;
    std::string artist;
    std::string title;
    double duration = 0.0;
    bool is_search = false;
};

SoupMessage* NewMessage(const std::string& url) {
    SoupMessage* message = soup_message_new("GET", url.c_str());
    if (message != nullptr) {
        soup_message_headers_append(soup_message_get_request_headers(message), "User-Agent",
                                    kUserAgent);
    }
    return message;
}

std::string BuildGetUrl(const std::string& artist, const std::string& title,
                        const std::string& album, double duration) {
    std::string url = "https://lrclib.net/api/get?track_name=" + Escape(title.c_str());
    const std::string escaped_artist = Escape(artist.c_str());
    if (!escaped_artist.empty()) url += "&artist_name=" + escaped_artist;
    const std::string escaped_album = Escape(album.c_str());
    if (!escaped_album.empty()) url += "&album_name=" + escaped_album;
    if (duration > 0.0) url += "&duration=" + std::to_string(static_cast<long long>(duration + 0.5));
    return url;
}

std::string BuildSearchUrl(const std::string& artist, const std::string& title) {
    std::string url = "https://lrclib.net/api/search?track_name=" + Escape(title.c_str());
    const std::string escaped_artist = Escape(artist.c_str());
    if (!escaped_artist.empty()) url += "&artist_name=" + escaped_artist;
    return url;
}

void ExtractFields(cJSON* object, lrclib_result* out, std::string* synced, std::string* plain,
                   std::string* track, std::string* artist, std::string* album) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, "syncedLyrics");
    if (cJSON_IsString(item) && item->valuestring != nullptr) *synced = item->valuestring;
    item = cJSON_GetObjectItemCaseSensitive(object, "plainLyrics");
    if (cJSON_IsString(item) && item->valuestring != nullptr) *plain = item->valuestring;
    item = cJSON_GetObjectItemCaseSensitive(object, "trackName");
    if (cJSON_IsString(item) && item->valuestring != nullptr) *track = item->valuestring;
    item = cJSON_GetObjectItemCaseSensitive(object, "artistName");
    if (cJSON_IsString(item) && item->valuestring != nullptr) *artist = item->valuestring;
    item = cJSON_GetObjectItemCaseSensitive(object, "albumName");
    if (cJSON_IsString(item) && item->valuestring != nullptr) *album = item->valuestring;
    item = cJSON_GetObjectItemCaseSensitive(object, "duration");
    if (cJSON_IsNumber(item)) out->duration = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(object, "instrumental");
    out->instrumental = cJSON_IsTrue(item) ? 1 : 0;
}

void Finish(RequestContext* context, lrclib_result* out, std::string* synced, std::string* plain,
            std::string* track, std::string* artist, std::string* album) {
    out->synced_lrc = synced->empty() ? nullptr : synced->c_str();
    out->plain_lyrics = plain->empty() ? nullptr : plain->c_str();
    out->track_name = track->c_str();
    out->artist_name = artist->c_str();
    out->album_name = album->c_str();

    if (context->callback != nullptr) context->callback(context->user_data, out);
    delete context;
}

void OnResponse(GObject* source, GAsyncResult* result, gpointer user_data) {
    auto* context = static_cast<RequestContext*>(user_data);
    SoupSession* session = SOUP_SESSION(source);

    GError* error = nullptr;
    GBytes* bytes = soup_session_send_and_read_finish(session, result, &error);

    const int status =
        (context->message != nullptr) ? static_cast<int>(soup_message_get_status(context->message)) : 0;
    if (context->message != nullptr) {
        g_object_unref(context->message);
        context->message = nullptr;
    }

    // /api/get has no exact match -> fall back to a fuzzy /api/search.
    if (bytes != nullptr && !context->is_search && status != 200) {
        g_bytes_unref(bytes);
        context->is_search = true;
        context->message = NewMessage(BuildSearchUrl(context->artist, context->title));
        if (context->message == nullptr) {
            lrclib_result empty{};
            empty.status = status;
            if (context->callback != nullptr) context->callback(context->user_data, &empty);
            delete context;
            return;
        }
        soup_session_send_and_read_async(session, context->message, G_PRIORITY_DEFAULT, nullptr,
                                         OnResponse, context);
        return;
    }

    lrclib_result out{};
    std::string synced;
    std::string plain;
    std::string track;
    std::string artist;
    std::string album;

    if (bytes == nullptr) {
        out.status = 0;
        if (context->callback != nullptr) context->callback(context->user_data, &out);
        g_clear_error(&error);
        delete context;
        return;
    }

    out.status = status;
    gsize size = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    cJSON* root = cJSON_ParseWithLength(data, size);

    if (root != nullptr) {
        if (context->is_search) {
            // Prefer a synced entry whose duration is closest to the query.
            cJSON* best = nullptr;
            double best_delta = 1e18;
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, root) {
                const cJSON* synced_item = cJSON_GetObjectItemCaseSensitive(item, "syncedLyrics");
                if (!cJSON_IsString(synced_item) || synced_item->valuestring == nullptr) continue;
                double duration = 0.0;
                const cJSON* duration_item = cJSON_GetObjectItemCaseSensitive(item, "duration");
                if (cJSON_IsNumber(duration_item)) duration = duration_item->valuedouble;
                const double delta =
                    (context->duration > 0.0) ? std::fabs(duration - context->duration) : 0.0;
                if (best == nullptr || delta < best_delta) {
                    best = item;
                    best_delta = delta;
                }
            }
            if (best == nullptr) {
                cJSON_ArrayForEach(item, root) {
                    const cJSON* plain_item = cJSON_GetObjectItemCaseSensitive(item, "plainLyrics");
                    if (cJSON_IsString(plain_item) && plain_item->valuestring != nullptr) {
                        best = item;
                        break;
                    }
                }
            }
            if (best != nullptr) ExtractFields(best, &out, &synced, &plain, &track, &artist, &album);
        } else {
            ExtractFields(root, &out, &synced, &plain, &track, &artist, &album);
        }
        cJSON_Delete(root);
    }

    Finish(context, &out, &synced, &plain, &track, &artist, &album);
    g_bytes_unref(bytes);
}

}  // namespace

extern "C" lrclib_client* lrclib_client_new(void) {
    auto* client = new lrclib_client();
    client->session = soup_session_new();
    g_object_set(client->session, "timeout", 10, nullptr);
    return client;
}

extern "C" void lrclib_client_free(lrclib_client* client) {
    if (client == nullptr) return;
    if (client->session != nullptr) g_object_unref(client->session);
    delete client;
}

extern "C" void lrclib_get(lrclib_client* client, const char* artist, const char* title,
                           const char* album, double duration_seconds,
                           lrclib_callback callback, void* user_data) {
    if (client == nullptr || title == nullptr || title[0] == '\0') {
        lrclib_result empty{};
        if (callback != nullptr) callback(user_data, &empty);
        return;
    }

    const std::string artist_str = (artist != nullptr) ? artist : "";
    const std::string title_str = title;
    const std::string album_str = (album != nullptr) ? album : "";

    SoupMessage* message = NewMessage(BuildGetUrl(artist_str, title_str, album_str, duration_seconds));
    if (message == nullptr) {
        lrclib_result empty{};
        if (callback != nullptr) callback(user_data, &empty);
        return;
    }

    auto* context =
        new RequestContext{message, callback, user_data, artist_str, title_str, duration_seconds, false};
    soup_session_send_and_read_async(client->session, message, G_PRIORITY_DEFAULT, nullptr, OnResponse,
                                     context);
}
