#ifndef RAYLYRICS_MEDIA_H
#define RAYLYRICS_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Unified playback state. Strings are owned by the backend and stay valid
// until the next media_poll() call.
typedef struct media_state {
    const char *title;
    const char *artist;
    const char *album;
    const char *track_id;
    const char *url;
    const char *player;          // D-Bus bus name

    bool valid;                  // a player is present and metadata was read
    bool playing;

    int64_t position_us;         // position as sampled from the player
    int64_t position_stamp_us;   // CLOCK_MONOTONIC timestamp of that sample
    int64_t length_us;           // track length, 0 if unknown
    double rate;                 // PlaybackRate

    uint64_t generation;         // bumped whenever the track changes
} media_state;

typedef struct media_backend media_backend;

// MPRIS backend over the session bus. Returns NULL on failure.
media_backend *media_mpris_create(void);
void media_mpris_destroy(media_backend *backend);

// Drain pending D-Bus events (non-blocking) and update the state.
void media_poll(media_backend *backend);

// Latest state, owned by the backend.
const media_state *media_get_state(const media_backend *backend);

// Extrapolate the playback position with CLOCK_MONOTONIC and PlaybackRate.
int64_t media_current_position_us(const media_state *state);

#ifdef __cplusplus
}
#endif

#endif  // RAYLYRICS_MEDIA_H
