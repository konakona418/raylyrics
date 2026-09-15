#include "lyrics/lrc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct lrc_doc {
    std::vector<lrc_line> lines;
    std::string title;
    std::string artist;
    std::string album;
    int64_t offset_us = 0;
};

namespace {

char* DupString(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

size_t Utf8Count(const char* text) {
    if (text == nullptr) return 0;
    size_t count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p) {
        if ((*p & 0xC0) != 0x80) count++;
    }
    return count;
}

// Parse "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx".
bool ParseClock(const std::string& text, int64_t* out_us) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return false;

    long minutes = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
        minutes = minutes * 10 + (text[i] - '0');
    }

    const std::string rest = text.substr(colon + 1);
    const size_t sep = rest.find_first_of(".:");
    const std::string seconds_text = (sep == std::string::npos) ? rest : rest.substr(0, sep);
    if (seconds_text.empty()) return false;

    long seconds = 0;
    for (char c : seconds_text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        seconds = seconds * 10 + (c - '0');
    }

    long millis = 0;
    if (sep != std::string::npos) {
        const std::string fraction = rest.substr(sep + 1);
        if (fraction.empty()) return false;
        long value = 0;
        int digits = 0;
        for (char c : fraction) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            value = value * 10 + (c - '0');
            digits++;
        }
        while (digits < 3) {
            value *= 10;
            digits++;
        }
        while (digits > 3) {
            value /= 10;
            digits--;
        }
        millis = value;
    }

    *out_us = (static_cast<int64_t>(minutes) * 60 + seconds) * 1000000 + static_cast<int64_t>(millis) * 1000;
    return true;
}

// Split a line body into display text plus optional enhanced-LRC syllables.
void ParseBody(const std::string& body, int64_t line_time, std::string* display,
               std::vector<lrc_syllable>* syllables) {
    std::vector<std::pair<int64_t, std::string>> parts;
    std::string current;
    int64_t current_time = line_time;
    bool has_tags = false;

    size_t i = 0;
    while (i < body.size()) {
        if (body[i] == '<') {
            const size_t close = body.find('>', i);
            if (close != std::string::npos) {
                const std::string tag = body.substr(i + 1, close - i - 1);
                int64_t time = 0;
                if (ParseClock(tag, &time)) {
                    parts.emplace_back(current_time, current);
                    current.clear();
                    current_time = time;
                    has_tags = true;
                    i = close + 1;
                    continue;
                }
            }
        }
        current += body[i++];
    }
    parts.emplace_back(current_time, current);

    if (!has_tags) {
        *display = body;
        return;
    }

    for (const auto& [time, text] : parts) {
        if (text.empty()) continue;
        *display += text;
        lrc_syllable syllable;
        syllable.time_us = time;
        syllable.text = DupString(text);
        syllables->push_back(syllable);
    }
}

}  // namespace

lrc_doc* lrc_parse(const char* text, size_t length) {
    if (text == nullptr || length == 0) return nullptr;

    auto* doc = new lrc_doc();
    const std::string content(text, length);

    size_t position = 0;
    while (position <= content.size()) {
        const size_t newline = content.find('\n', position);
        std::string line = content.substr(position, (newline == std::string::npos) ? std::string::npos
                                                                                 : newline - position);
        position = (newline == std::string::npos) ? content.size() + 1 : newline + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<int64_t> timestamps;

        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            const size_t close = line.find(']', i);
            if (close == std::string::npos) break;

            const std::string tag = line.substr(i + 1, close - i - 1);
            int64_t time = 0;
            if (ParseClock(tag, &time)) {
                timestamps.push_back(time);
            } else {
                const size_t colon = tag.find(':');
                if (colon != std::string::npos) {
                    const std::string key = tag.substr(0, colon);
                    const std::string value = tag.substr(colon + 1);
                    if (key == "ar") doc->artist = value;
                    else if (key == "ti") doc->title = value;
                    else if (key == "al") doc->album = value;
                    else if (key == "offset") doc->offset_us = std::atoll(value.c_str()) * 1000;
                }
            }
            i = close + 1;
        }

        if (timestamps.empty()) continue;

        const std::string body = line.substr(i);
        std::string display;
        std::vector<lrc_syllable> syllables;
        ParseBody(body, timestamps.front(), &display, &syllables);

        for (int64_t timestamp : timestamps) {
            lrc_line out{};
            out.time_us = timestamp;
            out.text = DupString(display);
            if (!syllables.empty()) {
                out.syllable_count = syllables.size();
                out.syllables = static_cast<lrc_syllable*>(std::malloc(sizeof(lrc_syllable) * syllables.size()));
                std::memcpy(out.syllables, syllables.data(), sizeof(lrc_syllable) * syllables.size());
            }
            doc->lines.push_back(out);
        }
    }

    if (doc->lines.empty()) {
        delete doc;
        return nullptr;
    }

    std::stable_sort(doc->lines.begin(), doc->lines.end(),
                     [](const lrc_line& a, const lrc_line& b) { return a.time_us < b.time_us; });

    if (doc->offset_us != 0) {
        for (lrc_line& line : doc->lines) line.time_us += doc->offset_us;
    }

    return doc;
}

void lrc_free(lrc_doc* doc) {
    if (doc == nullptr) return;
    for (lrc_line& line : doc->lines) {
        std::free(line.text);
        for (size_t i = 0; i < line.syllable_count; ++i) std::free(line.syllables[i].text);
        std::free(line.syllables);
    }
    delete doc;
}

size_t lrc_line_count(const lrc_doc* doc) { return doc ? doc->lines.size() : 0; }

const lrc_line* lrc_line_at(const lrc_doc* doc, size_t index) {
    if (doc == nullptr || index >= doc->lines.size()) return nullptr;
    return &doc->lines[index];
}

int lrc_line_index_at(const lrc_doc* doc, int64_t position_us) {
    if (doc == nullptr || doc->lines.empty()) return -1;
    const auto it = std::upper_bound(
        doc->lines.begin(), doc->lines.end(), position_us,
        [](int64_t value, const lrc_line& line) { return value < line.time_us; });
    return static_cast<int>(it - doc->lines.begin()) - 1;
}

size_t lrc_highlight_count(const lrc_doc* doc, size_t index, int64_t position_us) {
    if (doc == nullptr || index >= doc->lines.size()) return 0;
    const lrc_line& line = doc->lines[index];

    if (line.syllable_count > 0) {
        size_t count = 0;
        for (size_t i = 0; i < line.syllable_count; ++i) {
            if (line.syllables[i].time_us > position_us) break;
            count += Utf8Count(line.syllables[i].text);
        }
        return count;
    }

    const int64_t start = line.time_us;
    int64_t end = start + 3000000;
    if (index + 1 < doc->lines.size()) end = doc->lines[index + 1].time_us;
    if (end <= start) end = start + 3000000;

    const size_t total = Utf8Count(line.text);
    if (total == 0) return 0;
    if (position_us < start) return 0;
    if (position_us >= end) return total;

    // Highlight the character currently being sung, not just the completed
    // ones: floor(progress * total) would lag one character behind and would
    // never show the last character.
    size_t count = static_cast<size_t>(static_cast<int64_t>(total) * (position_us - start) /
                                       (end - start));
    if (count < total) count += 1;
    return count;
}

const char* lrc_title(const lrc_doc* doc) { return doc ? doc->title.c_str() : ""; }
const char* lrc_artist(const lrc_doc* doc) { return doc ? doc->artist.c_str() : ""; }
const char* lrc_album(const lrc_doc* doc) { return doc ? doc->album.c_str() : ""; }
int64_t lrc_offset_us(const lrc_doc* doc) { return doc ? doc->offset_us : 0; }
