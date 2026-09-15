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

typedef struct lrclib_client lrclib_client;

lrclib_client* lrclib_client_new(void);
void lrclib_client_free(lrclib_client* client);

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
