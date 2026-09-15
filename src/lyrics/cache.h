#ifndef RAYLYRICS_LYRICS_CACHE_H
#define RAYLYRICS_LYRICS_CACHE_H

#include <string>
#include <vector>

namespace raylyrics {

// On-disk cache of lyrics fetched from the network, keyed by track metadata.
// Each entry is <dir>/<key>.lrc (the lyrics) plus <dir>/<key>.json (metadata).
class LyricsCache {
public:
    struct Entry {
        std::string key;
        std::string artist;
        std::string title;
        std::string album;
        std::string source;
        double duration = 0.0;
        long size = 0;
        long mtime = 0;
    };

    // Entries older than the TTL are treated as missing and dropped on lookup.
    // A non-positive TTL disables expiry.
    static constexpr long kDefaultTtlSeconds = 30L * 24 * 60 * 60;

    explicit LyricsCache(std::string dir, long ttl_seconds = kDefaultTtlSeconds);

    // $XDG_CACHE_HOME/raylyrics/lyrics, or ~/.cache/raylyrics/lyrics.
    static std::string DefaultDir();

    // Stable hex key for a track; the duration is rounded to whole seconds.
    static std::string Key(const std::string& artist, const std::string& title,
                           const std::string& album, double duration);

    const std::string& dir() const { return dir_; }
    long ttl_seconds() const { return ttl_seconds_; }

    // Returns true and fills `out` on a hit.
    bool Get(const std::string& artist, const std::string& title, const std::string& album,
             double duration, std::string* out) const;

    // Stores lyrics; `source` is e.g. "lrclib".
    void Put(const std::string& artist, const std::string& title, const std::string& album,
             double duration, const std::string& lyrics, const std::string& source);

    // Removes one entry by key. Returns true if it existed.
    bool Remove(const std::string& key);

    // Removes everything. Returns the number of lyrics entries removed.
    int Clear();

    // Removes every expired entry. Returns the number of lyrics entries removed.
    int Prune();

    // All entries, sorted by artist then title.
    std::vector<Entry> List() const;

private:
    std::string PathFor(const std::string& key, const char* extension) const;

    std::string dir_;
    long ttl_seconds_ = kDefaultTtlSeconds;
};

}  // namespace raylyrics

#endif  // RAYLYRICS_LYRICS_CACHE_H
