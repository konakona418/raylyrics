#include "lyrics/match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "lyrics/title_grammar.h"

namespace raylyrics {
namespace {

constexpr double kDurationToleranceS = 3.0;
constexpr double kStrongTitleSimilarity = 0.85;

// Version tags that actually change the recording. A remaster has the same
// words and timings as the studio take, so it must not force a conflict that
// rejects the only correct candidate.
std::set<std::string> EffectiveTags(const std::string& title) {
    std::set<std::string> tags = ExtractVersionTags(title);
    for (auto it = tags.begin(); it != tags.end();) {
        if (IsLyricNeutralTag(*it)) {
            it = tags.erase(it);
        } else {
            ++it;
        }
    }
    return tags;
}

std::string JoinTags(const std::set<std::string>& tags) {
    if (tags.empty()) return "-";
    std::string out;
    for (const std::string& tag : tags) {
        if (!out.empty()) out += ",";
        out += tag;
    }
    return out;
}

// One title sitting inside the other, with the artist corroborating. Either
// direction counts: a browser upload wraps the catalogue name in decoration
// (candidate inside the query), and a truncated or abbreviated query is the
// same relationship the other way round. The shorter side must still be long
// enough to mean something.
bool ContainsFuzzy(const std::string& query_title, const std::string& candidate_title,
                   bool artist_overlap) {
    if (!artist_overlap) return false;
    const std::string query_text = NormalizeText(query_title);
    const std::string candidate_text = NormalizeText(BaseTitle(candidate_title));
    if (candidate_text.empty() || query_text.empty()) return false;

    const std::string& shorter =
        candidate_text.size() <= query_text.size() ? candidate_text : query_text;
    const std::string& longer =
        candidate_text.size() <= query_text.size() ? query_text : candidate_text;
    if (shorter.size() < 4) return false;
    return longer.find(shorter) != std::string::npos;
}

bool Better(const MatchEvidence& a, const MatchEvidence& b) {
    const int rank_a = static_cast<int>(a.confidence);
    const int rank_b = static_cast<int>(b.confidence);
    if (rank_a != rank_b) return rank_a > rank_b;
    if (std::fabs(a.similarity - b.similarity) > 1e-9) return a.similarity > b.similarity;
    const double delta_a = a.duration_delta < 0.0 ? 1e18 : a.duration_delta;
    const double delta_b = b.duration_delta < 0.0 ? 1e18 : b.duration_delta;
    return delta_a < delta_b;
}

std::string Describe(std::size_t index, const Candidate& candidate,
                     const MatchEvidence& evidence) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "#%zu %s - %s", index + 1, candidate.artist.c_str(),
                  candidate.title.c_str());
    std::string line = buffer;
    if (candidate.duration_s > 0.0) {
        std::snprintf(buffer, sizeof(buffer), " (%.0fs)", candidate.duration_s);
        line += buffer;
    }
    line += (evidence.confidence == MatchConfidence::None) ? ": rejected - " : ": accepted - ";
    line += evidence.reason;
    return line;
}

}  // namespace

MatchEvidence Evaluate(const TrackMetadata& query, const Candidate& candidate) {
    MatchEvidence evidence;

    // Gate 1: version conflict.
    const std::set<std::string> query_tags = EffectiveTags(query.title);
    const std::set<std::string> candidate_tags = EffectiveTags(candidate.title);
    if (query_tags != candidate_tags) {
        evidence.reason = "version conflict (query " + JoinTags(query_tags) + " vs candidate " +
                          JoinTags(candidate_tags) + ")";
        return evidence;
    }

    // Gate 2: no artist in common.
    const std::set<std::string> query_artists = ArtistTokens(query.artist);
    const std::set<std::string> candidate_artists = ArtistTokens(candidate.artist);
    if (!query_artists.empty() && !candidate_artists.empty()) {
        bool overlap = false;
        for (const std::string& token : query_artists) {
            if (candidate_artists.count(token) != 0) {
                overlap = true;
                break;
            }
        }
        if (!overlap) {
            evidence.reason = "no artist in common (\"" + query.artist + "\" vs \"" +
                              candidate.artist + "\")";
            return evidence;
        }
        evidence.artist_overlap = true;
    }

    const std::string query_base = NormalizeText(BaseTitle(query.title));
    const std::string candidate_base = NormalizeText(BaseTitle(candidate.title));
    evidence.title_exact = !query_base.empty() && query_base == candidate_base;
    evidence.similarity = TitleSimilarity(query_base, candidate_base);

    const std::string query_album = NormalizeText(query.album);
    evidence.album_match =
        !query_album.empty() && query_album == NormalizeText(candidate.album);

    if (query.duration_s > 0.0 && candidate.duration_s > 0.0) {
        evidence.duration_delta = std::fabs(query.duration_s - candidate.duration_s);
        evidence.duration_close = evidence.duration_delta <= kDurationToleranceS;
        // A disagreement larger than the whole shorter track is a container
        // length (an album upload), not a different edit.
        evidence.container_duration =
            evidence.duration_delta > std::min(query.duration_s, candidate.duration_s);
    }

    const bool strong_title =
        evidence.title_exact || evidence.similarity >= kStrongTitleSimilarity;

    if (evidence.title_exact && evidence.artist_overlap) {
        evidence.confidence = MatchConfidence::High;
        evidence.reason = "exact title and matching artist";
    } else if (strong_title &&
               (evidence.artist_overlap || evidence.album_match || evidence.duration_close)) {
        evidence.confidence = MatchConfidence::High;
        if (evidence.artist_overlap) {
            evidence.reason = "strong title, artist corroborates";
        } else if (evidence.album_match) {
            evidence.reason = "strong title, album corroborates";
        } else {
            evidence.reason = "strong title, duration within 3s";
        }
    } else if (evidence.title_exact && evidence.container_duration) {
        evidence.confidence = MatchConfidence::Medium;
        evidence.reason = "exact title, duration is a container length";
    } else if (strong_title) {
        evidence.confidence = MatchConfidence::Medium;
        evidence.reason = "strong title alone";
    } else if (ContainsFuzzy(query.title, candidate.title, evidence.artist_overlap)) {
        evidence.confidence = MatchConfidence::Medium;
        evidence.reason = "one title contains the other, artist corroborates";
    } else {
        evidence.confidence = MatchConfidence::None;
        evidence.reason = "nothing corroborated the title";
    }
    return evidence;
}

int Rank(const TrackMetadata& query, const std::vector<Candidate>& candidates,
         std::vector<std::string>* reasons) {
    const bool any_synced =
        std::any_of(candidates.begin(), candidates.end(),
                    [](const Candidate& candidate) { return candidate.has_synced; });

    int best = -1;
    MatchEvidence best_evidence;
    for (std::size_t i = 0; i < candidates.size(); i++) {
        const Candidate& candidate = candidates[i];
        MatchEvidence evidence = Evaluate(query, candidate);
        if (evidence.confidence != MatchConfidence::None) {
            if (any_synced && !candidate.has_synced) {
                evidence.confidence = MatchConfidence::None;
                evidence.reason = "no synced lyrics";
            } else if (!any_synced && !candidate.has_plain) {
                evidence.confidence = MatchConfidence::None;
                evidence.reason = "no lyrics";
            }
        }

        if (reasons != nullptr) reasons->push_back(Describe(i, candidate, evidence));
        if (evidence.confidence == MatchConfidence::None) continue;
        if (best < 0 || Better(evidence, best_evidence)) {
            best = static_cast<int>(i);
            best_evidence = evidence;
        }
    }
    return best;
}

}  // namespace raylyrics
