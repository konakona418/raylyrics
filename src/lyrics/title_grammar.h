#ifndef RAYLYRICS_LYRICS_TITLE_GRAMMAR_H
#define RAYLYRICS_LYRICS_TITLE_GRAMMAR_H

#include <set>
#include <string>

namespace raylyrics {

// Undo the grammar a publisher wraps around a song name: an upload title
// carries decoration no catalogue lists — bracketed credits, "Official MV",
// featured performers, a channel's own name fused in — and every matching path
// needs the same rules, whether the metadata arrived over MPRIS or from a
// browser bridge. Pure text; nothing here knows what a candidate is.

// Lowercase ASCII, keep alphanumerics and non-ASCII (CJK) bytes, turn every
// other byte into a space and collapse runs.
std::string NormalizeText(const std::string& text);

// The title with decoration removed: bracketed groups (unless a short one is
// glued to a word, as in "(G)I-DLE"), feat./ft. suffixes, platform noise such
// as "Official MV" or "中文字幕", and a trailing version marker.
std::string BaseTitle(const std::string& title);

// Version qualifier tags present in `text`; see the table in the .cpp.
std::set<std::string> ExtractVersionTags(const std::string& text);

// Tags that mark a different recording but the same words (a remaster, a
// choreography video). They must not count as a version conflict, or the only
// correct candidate gets rejected.
bool IsLyricNeutralTag(const std::string& tag);

// Artist identity: normalized tokens split on ",", "&", "feat." and friends,
// and the first (primary) one.
std::set<std::string> ArtistTokens(const std::string& artist);
std::string PrimaryArtist(const std::string& artist);

// Similarity in [0,1] between two strings, as the Dice coefficient over
// codepoint bigrams. Works for CJK, where word tokenization does not.
double TitleSimilarity(const std::string& a, const std::string& b);

}  // namespace raylyrics

#endif  // RAYLYRICS_LYRICS_TITLE_GRAMMAR_H
