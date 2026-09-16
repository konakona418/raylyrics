#include "lyrics/title_grammar.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace raylyrics {
namespace {

bool IsAsciiAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string ToLowerAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// UTF-8: a lead byte is anything that is not a continuation byte.
bool IsUtf8Lead(unsigned char c) { return (c & 0xC0) != 0x80; }

std::vector<std::string> Codepoints(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t length = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0xE0) == 0xC0) {
            length = 2;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
        }
        if (i + length > text.size()) length = 1;
        out.push_back(text.substr(i, length));
        i += length;
    }
    return out;
}

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) begin++;
    while (end > begin && IsSpace(text[end - 1])) end--;
    return text.substr(begin, end - begin);
}

std::string CollapseSpaces(const std::string& text) {
    std::string out;
    bool pending = false;
    for (char c : text) {
        if (IsSpace(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out += ' ';
            pending = false;
        }
        out += c;
    }
    return out;
}

bool IsAsciiMarker(const std::string& marker) {
    for (char c : marker) {
        if (IsAsciiLetter(c)) return true;
    }
    return false;
}

// Case-insensitive for ASCII markers, with ASCII-letter boundaries so "mv" does
// not match inside a word; literal for CJK.
bool MarkerPresent(const std::string& text, const std::string& marker) {
    if (!IsAsciiMarker(marker)) return text.find(marker) != std::string::npos;

    const std::string lower = ToLowerAscii(text);
    const std::string needle = ToLowerAscii(marker);
    std::size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        const bool left_ok = pos == 0 || !IsAsciiLetter(text[pos - 1]);
        const std::size_t after = pos + needle.size();
        const bool right_ok = after >= text.size() || !IsAsciiLetter(text[after]);
        if (left_ok && right_ok) return true;
        pos++;
    }
    return false;
}

void EraseMarker(std::string* text, const std::string& marker) {
    if (!IsAsciiMarker(marker)) {
        std::size_t pos = 0;
        while ((pos = text->find(marker, pos)) != std::string::npos) {
            text->replace(pos, marker.size(), " ");
            pos += 1;
        }
        return;
    }

    const std::string needle = ToLowerAscii(marker);
    std::size_t from = 0;
    while (from < text->size()) {
        const std::string lower = ToLowerAscii(*text);
        const std::size_t pos = lower.find(needle, from);
        if (pos == std::string::npos) break;
        const bool left_ok = pos == 0 || !IsAsciiLetter((*text)[pos - 1]);
        const std::size_t after = pos + needle.size();
        const bool right_ok = after >= text->size() || !IsAsciiLetter((*text)[after]);
        if (left_ok && right_ok) text->replace(pos, needle.size(), " ");
        from = pos + 1;
    }
}

// Decoration a publisher adds that is not part of the name.
struct Noise {
    const char* marker;
};

const std::vector<Noise>& NoiseTable() {
    static const std::vector<Noise> table = {
        {"official music video"}, {"official lyric video"}, {"official visualizer"},
        {"official audio"},       {"official video"},       {"official hd"},
        {"official mv"},          {"music video"},          {"lyric video"},
        {"visualizer"},           {"動態歌詞"},              {"动态歌词"},
        {"歌詞字幕"},              {"歌词字幕"},              {"中文字幕"},
        {"完整高清音質"},          {"完整高清音质"},          {"官方高畫質"},
        {"官方高画质"},            {"高清mv"},               {"官方mv"},
        {"高清"},
    };
    return table;
}

// Version qualifiers: which words in a title mark a different recording of the
// same song. A candidate carrying different ones from the track is a different
// recording, whose timings will not line up even when the words are the same.
struct VersionTag {
    const char* tag;
    std::vector<const char*> markers;
};

const std::vector<VersionTag>& VersionTable() {
    static const std::vector<VersionTag> table = {
        {"acoustic", {"acoustic", "acounstic", "unplugged", "原声版", "原聲版"}},
        {"cover", {"cover", "翻唱", "歌ってみた"}},
        {"alt_vocal", {"女声版", "女聲版", "男声版", "男聲版"}},
        {"demo", {"demo"}},
        {"edit", {"edit"}},
        {"extended", {"extended"}},
        {"instrumental",
         {"instrumental", "off vocal", "off-vocal", "伴奏", "钢琴版", "鋼琴版", "纯音乐版",
          "純音樂版", "纯音乐", "純音樂"}},
        {"karaoke", {"karaoke", "卡拉ok"}},
        {"tv_size", {"tv size", "tv-size", "tvsize", "tv ver", "tvサイズ", "テレビサイズ"}},
        {"live", {"live", "live版", "现场", "現場"}},
        {"remaster", {"remaster", "remastered"}},
        {"remix", {"remix", "dj版"}},
        {"guitar", {"吉他版"}},
        {"strum", {"弹唱版", "彈唱版"}},
        {"opera", {"戏腔版", "戲腔版"}},
        {"cantonese", {"粤语版", "粵語版"}},
        {"sped_up", {"sped up", "sped-up", "spedup", "加速版"}},
        {"slowed",
         {"slowed down", "slowed + reverb", "slowed and reverb", "slowed", "慢速版", "降速版"}},
        {"reverb", {"reverb", "reverbed"}},
        {"nightcore", {"nightcore"}},
        {"rnb", {"r&b版"}},
        {"smoky", {"烟嗓版", "煙嗓版"}},
        {"full", {"full version"}},
        {"opening", {"opening title version"}},
    };
    return table;
}

const std::vector<std::string>& AllMarkers() {
    static const std::vector<std::string> markers = [] {
        std::vector<std::string> out;
        for (const VersionTag& tag : VersionTable()) {
            for (const char* marker : tag.markers) out.emplace_back(marker);
        }
        // Longest first, so "slowed down" is cut before "slowed".
        std::sort(out.begin(), out.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        return out;
    }();
    return markers;
}

const std::vector<std::string>& Brackets() {
    // Open/close pairs by index.
    static const std::vector<std::string> brackets = {"(", ")", "（", "）", "[", "]", "【",
                                                      "】", "『", "』"};
    return brackets;
}

bool IsOpenBracket(const std::string& codepoint, int* pair) {
    const std::vector<std::string>& brackets = Brackets();
    for (int i = 0; i < static_cast<int>(brackets.size()); i += 2) {
        if (codepoint == brackets[static_cast<std::size_t>(i)]) {
            *pair = i;
            return true;
        }
    }
    return false;
}

// Drops bracketed groups, except a short one glued to a following word, as in
// the artist name (G)I-DLE.
std::string StripBrackets(const std::string& text) {
    const std::vector<std::string>& brackets = Brackets();
    const std::vector<std::string> codepoints = Codepoints(text);
    std::string out;
    std::size_t i = 0;
    while (i < codepoints.size()) {
        int pair = -1;
        if (!IsOpenBracket(codepoints[i], &pair)) {
            out += codepoints[i];
            i++;
            continue;
        }

        std::size_t close = i + 1;
        while (close < codepoints.size() &&
               codepoints[close] != brackets[static_cast<std::size_t>(pair) + 1]) {
            close++;
        }
        if (close >= codepoints.size()) {
            out += codepoints[i];
            i++;
            continue;
        }

        const std::size_t inner = close - i - 1;
        const bool glued = close + 1 < codepoints.size() && codepoints[close + 1].size() == 1 &&
                           IsAsciiAlnum(codepoints[close + 1][0]);
        if (glued && inner <= 3) {
            for (std::size_t k = i; k <= close; k++) out += codepoints[k];
        }
        i = close + 1;
    }
    return out;
}

std::string StripFeatSuffix(const std::string& text) {
    const std::vector<std::string> markers = {"featuring", "feat.", "feat", "ft.", "ft"};
    const std::string lower = ToLowerAscii(text);
    std::size_t cut = std::string::npos;
    for (const std::string& marker : markers) {
        std::size_t pos = 0;
        while ((pos = lower.find(marker, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || !IsAsciiLetter(text[pos - 1]);
            const std::size_t after = pos + marker.size();
            const bool right_ok = after >= text.size() || !IsAsciiLetter(text[after]);
            if (left_ok && right_ok && pos > 0 && pos < cut) cut = pos;
            pos++;
        }
    }
    return cut == std::string::npos ? text : Trim(text.substr(0, cut));
}

// Only a marker the name ends on qualifies it: "Live and Learn" must not be
// tagged `live`, or the live recording outranks the studio one being played.
std::string StripTrailingVersionMarker(const std::string& text) {
    const std::string lower = ToLowerAscii(text);
    std::size_t cut = std::string::npos;
    for (const std::string& marker : AllMarkers()) {
        const bool ascii = IsAsciiMarker(marker);
        const std::string needle = ascii ? ToLowerAscii(marker) : marker;
        const std::string& haystack = ascii ? lower : text;
        std::size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            bool valid = true;
            if (ascii) {
                const bool left_ok = pos == 0 || !IsAsciiLetter(text[pos - 1]);
                const std::size_t after = pos + needle.size();
                const bool right_ok = after >= text.size() || !IsAsciiLetter(text[after]);
                valid = left_ok && right_ok;
            }
            const std::size_t end = pos + needle.size();
            if (valid && pos > 0 && Trim(text.substr(end)).empty() && pos < cut) cut = pos;
            pos++;
        }
    }
    return cut == std::string::npos ? text : Trim(text.substr(0, cut));
}

const std::vector<std::string>& ArtistSeparators() {
    static const std::vector<std::string> separators = {
        ",", "，", "、", "&", "/", ";", "|", "×", "•", "·",
        " feat", " ft", " featuring", " with",
    };
    return separators;
}

std::size_t FirstSeparator(const std::string& artist) {
    std::size_t cut = std::string::npos;
    for (const std::string& separator : ArtistSeparators()) {
        const std::size_t pos = artist.find(separator);
        if (pos != std::string::npos && pos < cut) cut = pos;
    }
    return cut;
}

}  // namespace

std::string NormalizeText(const std::string& text) {
    std::string out;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) {
            out += c;  // keep UTF-8 bytes (CJK, accents) as they are
            continue;
        }
        if (IsAsciiAlnum(c)) {
            out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            continue;
        }
        out += ' ';
    }
    return CollapseSpaces(Trim(out));
}

std::set<std::string> ExtractVersionTags(const std::string& text) {
    std::set<std::string> tags;
    for (const VersionTag& tag : VersionTable()) {
        for (const char* marker : tag.markers) {
            if (MarkerPresent(text, marker)) {
                tags.insert(tag.tag);
                break;
            }
        }
    }
    return tags;
}

bool IsLyricNeutralTag(const std::string& tag) {
    return tag == "remaster" || tag == "choreography";
}

std::string BaseTitle(const std::string& title) {
    std::string value = title;
    for (const Noise& noise : NoiseTable()) EraseMarker(&value, noise.marker);
    value = CollapseSpaces(value);

    std::string base = CollapseSpaces(StripBrackets(value));
    // A title wholly inside brackets is the name, not a qualifier.
    if (base.empty()) base = CollapseSpaces(value);

    base = StripFeatSuffix(base);
    base = StripTrailingVersionMarker(base);
    base = CollapseSpaces(Trim(base));
    return base.empty() ? Trim(title) : base;
}

std::set<std::string> ArtistTokens(const std::string& artist) {
    std::string value = artist;
    for (const std::string& separator : ArtistSeparators()) {
        std::size_t pos = 0;
        while ((pos = value.find(separator, pos)) != std::string::npos) {
            value.replace(pos, separator.size(), "\x1f");
            pos += 1;
        }
    }

    std::set<std::string> tokens;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\x1f', start);
        const std::string piece =
            NormalizeText(value.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start));
        if (!piece.empty() && piece != "feat" && piece != "ft" && piece != "featuring" &&
            piece != "with") {
            tokens.insert(piece);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return tokens;
}

std::string PrimaryArtist(const std::string& artist) {
    const std::size_t cut = FirstSeparator(artist);
    return NormalizeText(cut == std::string::npos ? artist : artist.substr(0, cut));
}

double TitleSimilarity(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    const std::vector<std::string> left = Codepoints(a);
    const std::vector<std::string> right = Codepoints(b);
    if (left.size() < 2 || right.size() < 2) return 0.0;

    std::multiset<std::string> bigrams;
    for (std::size_t i = 0; i + 1 < right.size(); i++) {
        bigrams.insert(right[i] + right[i + 1]);
    }

    std::size_t common = 0;
    for (std::size_t i = 0; i + 1 < left.size(); i++) {
        const auto it = bigrams.find(left[i] + left[i + 1]);
        if (it != bigrams.end()) {
            bigrams.erase(it);
            common++;
        }
    }

    const std::size_t total = (left.size() - 1) + (right.size() - 1);
    return total == 0 ? 0.0 : (2.0 * static_cast<double>(common)) / static_cast<double>(total);
}

}  // namespace raylyrics
