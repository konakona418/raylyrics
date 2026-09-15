#ifndef RAYLYRICS_LYRICS_LRCLIB_H
#define RAYLYRICS_LYRICS_LRCLIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lrclib_result {
    int status;               // HTTP status, 0 on network/transport error
    const char* synced_lrc;   // LRC text, or NULL
    const char* plain_lyrics; // plain text, or NULL
    const char* track_name;
    const char* artist_name;
    const char* album_name;
    double duration;          // seconds
    int instrumental;
} lrclib_result;

typedef void (*lrclib_callback)(void* user_data, const lrclib_result* result);

// One /api/search candidate, offered to the selector below.
typedef struct lrclib_candidate {
    const char* track_name;
    const char* artist_name;
    const char* album_name;
    double duration;    // seconds, 0 if unknown
    int has_synced;
    int has_plain;
} lrclib_candidate;

// Picks a /api/search candidate. Return a 0-based index, or -1 to fall back to
// the built-in heuristic (closest synced duration, else first with plain text).
// The query and candidate strings are only valid for the duration of the call.
typedef int (*lrclib_selector)(void* user_data, const char* artist, const char* title,
                               const char* album, double query_duration,
                               const lrclib_candidate* candidates, int count);

typedef struct lrclib_client lrclib_client;

lrclib_client* lrclib_client_new(void);
void lrclib_client_free(lrclib_client* client);

// Install the /api/search candidate selector (optional).
void lrclib_set_selector(lrclib_client* client, lrclib_selector selector, void* user_data);

// Non-blocking GET /api/get. The callback runs on the thread that iterates the
// default GMainContext, and the strings in `result` are only valid for the
// duration of the callback.
void lrclib_get(lrclib_client* client, const char* artist, const char* title,
                const char* album, double duration_seconds, lrclib_callback callback,
                void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // RAYLYRICS_LYRICS_LRCLIB_H
