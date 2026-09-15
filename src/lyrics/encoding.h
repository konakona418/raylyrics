#ifndef RAYLYRICS_LYRICS_ENCODING_H
#define RAYLYRICS_LYRICS_ENCODING_H

#include <string>

namespace raylyrics {

// Convert lyric file bytes to UTF-8. Handles a UTF-8/UTF-16 BOM, passes valid
// UTF-8 through, and falls back to GB18030 (common for Chinese .lrc files).
// Invalid input is returned unchanged.
std::string ToUtf8(const std::string& bytes);

}  // namespace raylyrics

#endif  // RAYLYRICS_LYRICS_ENCODING_H
