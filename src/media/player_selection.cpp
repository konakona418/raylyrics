#include "media/player_selection.h"

#include <tuple>

namespace raylyrics {
namespace {

double PlayingSince(const std::map<std::string, double>& history, const std::string& name) {
    const auto it = history.find(name);
    return it == history.end() ? -1.0 : it->second;
}

// Rank a followable candidate: metadata first, then recency, then whether it is
// playing, then continuity.
//
// A source that names no performer cannot match anything, so it must not win on
// being the one currently playing: a browser exposes its own MPRIS service
// beside the bridge that wraps the same playback, and the browser's copy often
// carries the raw tab title with an empty artist while reporting a *different*
// media session as playing. Richness first keeps the overlay on the source that
// can actually be matched, and makes the choice stable across play/pause.
std::tuple<int, int, int, int, int> Score(const PlayerSnapshot& player,
                                          const std::map<std::string, double>& history,
                                          const std::string& current) {
    const double current_started = PlayingSince(history, current);
    const double candidate_started = PlayingSince(history, player.bus_name);
    const bool started_more_recently =
        current_started >= 0.0 && candidate_started >= 0.0 &&
        candidate_started - current_started > PlayerSelector::kRecentPlayerMargin;
    return {player.has_artist ? 1 : 0, started_more_recently ? 1 : 0, player.playing ? 1 : 0,
            player.has_title ? 1 : 0, player.bus_name == current ? 1 : 0};
}

}  // namespace

void PlayerSelector::Reset() {
    current_.clear();
    playing_since_.clear();
}

void PlayerSelector::Observe(const std::string& bus_name, bool playing, double at) {
    if (playing) {
        playing_since_.emplace(bus_name, at);  // keep the first sighting
    } else {
        playing_since_.erase(bus_name);
    }
}

void PlayerSelector::ForgetAbsent(const std::set<std::string>& present) {
    for (auto it = playing_since_.begin(); it != playing_since_.end();) {
        if (present.count(it->first) == 0) {
            it = playing_since_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string PlayerSelector::Choose(const std::vector<PlayerSnapshot>& players) const {
    // Take the richest player that reports a track. Richness (naming an artist)
    // outranks being the one currently playing, so two services wrapping the
    // same playback cannot make the overlay flip on every play/pause.
    const PlayerSnapshot* best = nullptr;
    std::tuple<int, int, int, int, int> best_score{};
    for (const PlayerSnapshot& player : players) {
        if (!player.followable || !player.has_track) continue;
        const std::tuple<int, int, int, int, int> score = Score(player, playing_since_, current_);
        if (best == nullptr || score > best_score) {
            best = &player;
            best_score = score;
        }
    }
    if (best != nullptr) return best->bus_name;

    // No player reports a track: fall back to one that is at least playing.
    for (const PlayerSnapshot& player : players) {
        if (player.followable && player.playing) return player.bus_name;
    }
    return {};
}

}  // namespace raylyrics
