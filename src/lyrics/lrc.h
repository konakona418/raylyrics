#ifndef RAYLYRICS_LYRICS_LRC_H
#define RAYLYRICS_LYRICS_LRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A timed fragment inside a line (enhanced LRC `<mm:ss.xx>` tags).
typedef struct lrc_syllable {
    int64_t time_us;
    char* text;  // UTF-8
} lrc_syllable;

typedef struct lrc_line {
    int64_t time_us;
    char* text;               // UTF-8, timing tags stripped
    lrc_syllable* syllables;  // NULL when the line has no word timings
    size_t syllable_count;
} lrc_line;

typedef struct lrc_doc lrc_doc;

// Parse UTF-8 LRC text. Returns NULL if nothing usable was found.
lrc_doc* lrc_parse(const char* text, size_t length);
void lrc_free(lrc_doc* doc);

size_t lrc_line_count(const lrc_doc* doc);
const lrc_line* lrc_line_at(const lrc_doc* doc, size_t index);

// Index of the active line at `position_us`, or -1 if before the first line.
int lrc_line_index_at(const lrc_doc* doc, int64_t position_us);

// Number of leading UTF-8 codepoints of line `index` that should be
// highlighted at `position_us` (0..codepoint count of the line).
size_t lrc_highlight_count(const lrc_doc* doc, size_t index, int64_t position_us);

const char* lrc_title(const lrc_doc* doc);
const char* lrc_artist(const lrc_doc* doc);
const char* lrc_album(const lrc_doc* doc);
int64_t lrc_offset_us(const lrc_doc* doc);

#ifdef __cplusplus
}
#endif

#endif  // RAYLYRICS_LYRICS_LRC_H
