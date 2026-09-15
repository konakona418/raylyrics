#ifndef RAYLYRICS_MEDIA_H
#define RAYLYRICS_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Unified playback state of the active player. Strings are owned by the
// backend and stay valid until the next media_poll() / selection change.
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

    uint64_t generation;         // bumped whenever the active track or player changes
} media_state;

// One entry per MPRIS player currently on the session bus.
typedef struct media_player_info {
    const char *name;            // D-Bus bus name
    const char *identity;        // org.mpris.MediaPlayer2.Identity (may be "")
    const char *title;
    const char *artist;
    const char *album;
    bool valid;
    bool playing;
    int64_t position_us;
    int64_t length_us;
} media_player_info;

typedef struct media_backend media_backend;

// MPRIS backend over the session bus. Returns NULL on failure.
media_backend *media_mpris_create(void);
void media_mpris_destroy(media_backend *backend);

// Drain pending D-Bus events (non-blocking) and refresh the active state.
void media_poll(media_backend *backend);

// Latest state of the active player, owned by the backend.
const media_state *media_get_state(const media_backend *backend);

// Extrapolate the playback position with CLOCK_MONOTONIC and PlaybackRate.
int64_t media_current_position_us(const media_state *state);

// All known players.
int media_player_count(const media_backend *backend);
const media_player_info *media_player_at(const media_backend *backend, int index);

// True when the player set, or any player's status/metadata, changed since the
// last media_set_active() call.
bool media_selection_dirty(const media_backend *backend);

// Choose the active player by bus name. NULL or "" applies the built-in
// fallback policy (keep a playing active player, else Firefox-first, else the
// first player). Clears the dirty flag.
void media_set_active(media_backend *backend, const char *name);

// Bus name of the active player, or "" when none is selected.
const char *media_active_player(const media_backend *backend);

#ifdef __cplusplus
}
#endif

#endif  // RAYLYRICS_MEDIA_H
