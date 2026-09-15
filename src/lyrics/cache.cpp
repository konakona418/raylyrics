#include "lyrics/cache.h"

#include <cjson/cJSON.h>
#include <glib.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace raylyrics {
namespace {

std::string CacheRoot() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/raylyrics";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.cache/raylyrics";
}

bool ReadFile(const std::string& path, std::string* out) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    out->clear();
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out->append(buffer, read);
    }
    std::fclose(file);
    return true;
}

bool WriteFile(const std::string& path, const std::string& data) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return written == data.size();
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) return item->valuestring;
    return {};
}

bool HasSuffix(const std::string& value, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

long NowSeconds() { return static_cast<long>(g_get_real_time() / 1000000); }

}  // namespace

LyricsCache::LyricsCache(std::string dir, long ttl_seconds)
    : dir_(std::move(dir)), ttl_seconds_(ttl_seconds) {}

std::string LyricsCache::DefaultDir() { return CacheRoot() + "/lyrics"; }

std::string LyricsCache::Key(const std::string& artist, const std::string& title,
                             const std::string& album, double duration) {
    const long long seconds = static_cast<long long>(duration + 0.5);
    const std::string material =
        artist + '\x1f' + title + '\x1f' + album + '\x1f' + std::to_string(seconds);

    // FNV-1a 64-bit.
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : material) {
        hash ^= c;
        hash *= 1099511628211ull;
    }

    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
    return out;
}

std::string LyricsCache::PathFor(const std::string& key, const char* extension) const {
    return dir_ + "/" + key + extension;
}

bool LyricsCache::Get(const std::string& artist, const std::string& title, const std::string& album,
                      double duration, std::string* out) const {
    const std::string key = Key(artist, title, album, duration);
    const std::string path = PathFor(key, ".lrc");

    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    if (ttl_seconds_ > 0 && NowSeconds() - static_cast<long>(info.st_mtime) > ttl_seconds_) {
        // Expired: drop it so the next request refetches.
        std::remove(path.c_str());
        std::remove(PathFor(key, ".json").c_str());
        return false;
    }
    return ReadFile(path, out);
}

void LyricsCache::Put(const std::string& artist, const std::string& title, const std::string& album,
                      double duration, const std::string& lyrics, const std::string& source) {
    g_mkdir_with_parents(dir_.c_str(), 0700);
    const std::string key = Key(artist, title, album, duration);
    if (!WriteFile(PathFor(key, ".lrc"), lyrics)) return;

    cJSON* meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "artist", artist.c_str());
    cJSON_AddStringToObject(meta, "title", title.c_str());
    cJSON_AddStringToObject(meta, "album", album.c_str());
    cJSON_AddNumberToObject(meta, "duration", duration);
    cJSON_AddStringToObject(meta, "source", source.c_str());
    cJSON_AddNumberToObject(meta, "time", static_cast<double>(g_get_real_time() / 1000000));
    char* text = cJSON_PrintUnformatted(meta);
    if (text != nullptr) {
        WriteFile(PathFor(key, ".json"), text);
        cJSON_free(text);
    }
    cJSON_Delete(meta);
}

bool LyricsCache::Remove(const std::string& key) {
    if (key.empty() || key.find('/') != std::string::npos) return false;
    const bool removed = std::remove(PathFor(key, ".lrc").c_str()) == 0;
    std::remove(PathFor(key, ".json").c_str());
    return removed;
}

int LyricsCache::Clear() {
    int removed = 0;
    GDir* dir = g_dir_open(dir_.c_str(), 0, nullptr);
    if (dir == nullptr) return 0;
    const gchar* name = nullptr;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        const std::string filename(name);
        const bool lyrics = HasSuffix(filename, ".lrc");
        if (!lyrics && !HasSuffix(filename, ".json")) continue;
        if (std::remove((dir_ + "/" + filename).c_str()) == 0 && lyrics) removed++;
    }
    g_dir_close(dir);
    return removed;
}

int LyricsCache::Prune() {
    if (ttl_seconds_ <= 0) return 0;
    int removed = 0;
    const long now = NowSeconds();
    for (const Entry& entry : List()) {
        if (now - entry.mtime > ttl_seconds_ && Remove(entry.key)) removed++;
    }
    return removed;
}

std::vector<LyricsCache::Entry> LyricsCache::List() const {
    std::vector<Entry> entries;
    GDir* dir = g_dir_open(dir_.c_str(), 0, nullptr);
    if (dir == nullptr) return entries;

    const gchar* name = nullptr;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        const std::string filename(name);
        if (!HasSuffix(filename, ".json")) continue;
        const std::string key = filename.substr(0, filename.size() - 5);

        std::string meta;
        if (!ReadFile(PathFor(key, ".json"), &meta)) continue;
        cJSON* root = cJSON_ParseWithLength(meta.data(), meta.size());
        if (root == nullptr) continue;

        Entry entry;
        entry.key = key;
        entry.artist = JsonString(root, "artist");
        entry.title = JsonString(root, "title");
        entry.album = JsonString(root, "album");
        entry.source = JsonString(root, "source");
        const cJSON* duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
        if (cJSON_IsNumber(duration)) entry.duration = duration->valuedouble;
        cJSON_Delete(root);

        struct stat info;
        if (stat(PathFor(key, ".lrc").c_str(), &info) != 0) continue;  // orphaned metadata
        entry.size = static_cast<long>(info.st_size);
        entry.mtime = static_cast<long>(info.st_mtime);
        entries.push_back(std::move(entry));
    }
    g_dir_close(dir);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.artist != b.artist) return a.artist < b.artist;
        return a.title < b.title;
    });
    return entries;
}

}  // namespace raylyrics
