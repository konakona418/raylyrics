#include "lyrics/lrclib.h"

#include <cjson/cJSON.h>
#include <libsoup/soup.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

struct lrclib_client {
    SoupSession* session;
    lrclib_selector selector = nullptr;
    void* selector_data = nullptr;
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
    std::string album;
    double duration = 0.0;
    bool is_search = false;
    lrclib_selector selector = nullptr;
    void* selector_data = nullptr;
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

// Built-in /api/search heuristic: the synced entry whose duration is closest
// to the query, else the first entry that has plain lyrics.
int BuiltinSearchChoice(const std::vector<cJSON*>& items, double query_duration) {
    int best = -1;
    double best_delta = 1e18;
    for (size_t i = 0; i < items.size(); i++) {
        const cJSON* synced_item = cJSON_GetObjectItemCaseSensitive(items[i], "syncedLyrics");
        if (!cJSON_IsString(synced_item) || synced_item->valuestring == nullptr) continue;
        double duration = 0.0;
        const cJSON* duration_item = cJSON_GetObjectItemCaseSensitive(items[i], "duration");
        if (cJSON_IsNumber(duration_item)) duration = duration_item->valuedouble;
        const double delta = (query_duration > 0.0) ? std::fabs(duration - query_duration) : 0.0;
        if (best < 0 || delta < best_delta) {
            best = static_cast<int>(i);
            best_delta = delta;
        }
    }
    if (best >= 0) return best;

    for (size_t i = 0; i < items.size(); i++) {
        const cJSON* plain_item = cJSON_GetObjectItemCaseSensitive(items[i], "plainLyrics");
        if (cJSON_IsString(plain_item) && plain_item->valuestring != nullptr) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Offers every /api/search entry to the selector (if installed) and returns the
// chosen index, falling back to BuiltinSearchChoice.
int ChooseSearchResult(const RequestContext* context, const std::vector<cJSON*>& items) {
    if (items.empty()) return -1;

    std::vector<std::string> tracks(items.size());
    std::vector<std::string> artists(items.size());
    std::vector<std::string> albums(items.size());
    std::vector<lrclib_candidate> candidates(items.size());

    for (size_t i = 0; i < items.size(); i++) {
        const cJSON* track = cJSON_GetObjectItemCaseSensitive(items[i], "trackName");
        if (cJSON_IsString(track) && track->valuestring != nullptr) tracks[i] = track->valuestring;
        const cJSON* artist = cJSON_GetObjectItemCaseSensitive(items[i], "artistName");
        if (cJSON_IsString(artist) && artist->valuestring != nullptr) artists[i] = artist->valuestring;
        const cJSON* album = cJSON_GetObjectItemCaseSensitive(items[i], "albumName");
        if (cJSON_IsString(album) && album->valuestring != nullptr) albums[i] = album->valuestring;
        const cJSON* duration = cJSON_GetObjectItemCaseSensitive(items[i], "duration");
        candidates[i].duration = cJSON_IsNumber(duration) ? duration->valuedouble : 0.0;
        const cJSON* synced = cJSON_GetObjectItemCaseSensitive(items[i], "syncedLyrics");
        candidates[i].has_synced = cJSON_IsString(synced) && synced->valuestring != nullptr;
        const cJSON* plain = cJSON_GetObjectItemCaseSensitive(items[i], "plainLyrics");
        candidates[i].has_plain = cJSON_IsString(plain) && plain->valuestring != nullptr;
        candidates[i].track_name = tracks[i].c_str();
        candidates[i].artist_name = artists[i].c_str();
        candidates[i].album_name = albums[i].c_str();
    }

    if (context->selector != nullptr) {
        const int chosen = context->selector(
            context->selector_data, context->artist.c_str(), context->title.c_str(),
            context->album.c_str(), context->duration, candidates.data(),
            static_cast<int>(candidates.size()));
        if (chosen >= 0 && chosen < static_cast<int>(items.size())) return chosen;
    }
    return BuiltinSearchChoice(items, context->duration);
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
            std::vector<cJSON*> items;
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, root) items.push_back(item);
            const int chosen = ChooseSearchResult(context, items);
            if (chosen >= 0) {
                ExtractFields(items[static_cast<size_t>(chosen)], &out, &synced, &plain, &track,
                              &artist, &album);
            }
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

    auto* context = new RequestContext{};
    context->message = message;
    context->callback = callback;
    context->user_data = user_data;
    context->artist = artist_str;
    context->title = title_str;
    context->album = album_str;
    context->duration = duration_seconds;
    context->is_search = false;
    context->selector = client->selector;
    context->selector_data = client->selector_data;
    soup_session_send_and_read_async(client->session, message, G_PRIORITY_DEFAULT, nullptr, OnResponse,
                                     context);
}

extern "C" void lrclib_set_selector(lrclib_client* client, lrclib_selector selector,
                                    void* user_data) {
    if (client == nullptr) return;
    client->selector = selector;
    client->selector_data = user_data;
}
