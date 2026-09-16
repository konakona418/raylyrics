#ifndef RAYLYRICS_LYRICS_MATCH_H
#define RAYLYRICS_LYRICS_MATCH_H

#include <string>
#include <vector>

namespace raylyrics {

// Rank a provider's candidates against the track that is playing.
//
// Two gates come first and nothing passes them: a version conflict (a live
// take, a cover, an instrumental cut, 女声版 — a different recording whose
// timings will not line up), and no artist in common. Past those, the strength
// of the answer follows what corroborates the title.

struct TrackMetadata {
    std::string title;
    std::string artist;
    std::string album;
    double duration_s = 0.0;  // 0 = unknown
};

struct Candidate {
    std::string title;
    std::string artist;
    std::string album;
    double duration_s = 0.0;
    bool has_synced = false;
    bool has_plain = false;
};

enum class MatchConfidence { None = 0, Medium = 1, High = 2 };

struct MatchEvidence {
    MatchConfidence confidence = MatchConfidence::None;
    bool title_exact = false;
    bool artist_overlap = false;
    bool album_match = false;
    bool duration_close = false;
    bool container_duration = false;
    double similarity = 0.0;
    double duration_delta = -1.0;  // < 0 = unknown

    // Which rule settled this candidate, refusal included. The decision is a
    // ladder of conditions, so without a name for the rung that answered,
    // "no lyrics" cannot say what it was that did not line up.
    std::string reason;
};

MatchEvidence Evaluate(const TrackMetadata& query, const Candidate& candidate);

// Index of the best candidate, or -1 when none passes. When any candidate has
// synced lyrics, only those are considered. `reasons` (optional) receives one
// line per candidate, in the order they were given.
int Rank(const TrackMetadata& query, const std::vector<Candidate>& candidates,
         std::vector<std::string>* reasons);

}  // namespace raylyrics

#endif  // RAYLYRICS_LYRICS_MATCH_H
