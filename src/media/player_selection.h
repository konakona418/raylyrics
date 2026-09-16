#ifndef RAYLYRICS_MEDIA_PLAYER_SELECTION_H
#define RAYLYRICS_MEDIA_PLAYER_SELECTION_H

#include <map>
#include <set>
#include <string>
#include <vector>

namespace raylyrics {

// One player as it currently looks on the bus.
struct PlayerSnapshot {
    std::string bus_name;
    bool playing = false;
    bool followable = true;  // PlaybackStatus is Playing or Paused (not Stopped)
    bool has_track = false;  // a title or an artist is known
    bool has_artist = false;
    bool has_title = false;
};

// Which MPRIS player the overlay follows, plus the little history that decision
// needs. Pure policy: no D-Bus, no I/O, so the awkward combinations (two
// services wrapping the same playback, a paused player that reports metadata
// next to a playing one that does not) can be exercised without a session bus.
class PlayerSelector {
public:
    // How much later a rival must have started playing before the overlay
    // follows it instead, and how long a just-paused current player is held
    // before a still-playing rival may take over. Two services that wrap the
    // same playback announce themselves moments apart, and without the margin
    // the overlay flips between them on every play/pause.
    static constexpr double kRecentPlayerMargin = 1.0;

    void Reset();

    // Record whether a player is playing, and since when (monotonic seconds).
    void Observe(const std::string& bus_name, bool playing, double at);

    // Drop the history of players that are no longer on the bus.
    void ForgetAbsent(const std::set<std::string>& present);

    // The bus name to follow, or "" when nothing qualifies.
    std::string Choose(const std::vector<PlayerSnapshot>& players) const;

    const std::string& current() const { return current_; }
    void set_current(const std::string& name) { current_ = name; }

private:
    std::string current_;
    std::map<std::string, double> playing_since_;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_MEDIA_PLAYER_SELECTION_H
