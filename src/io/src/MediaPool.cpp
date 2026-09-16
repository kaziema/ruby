#include "ruby/io/MediaPool.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace ruby::io {
namespace {

using json = nlohmann::json;
using ruby::core::MediaKind;

// Names, not integers, as in the project format: avoids reinterpreting old files if
// values shift.
const char* name(MediaKind k) {
    switch (k) {
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Image: return "image";
        case MediaKind::Unknown: return "unknown";
    }
    return "unknown";
}

MediaKind mediaKind(const std::string& s) {
    if (s == "video") return MediaKind::Video;
    if (s == "audio") return MediaKind::Audio;
    if (s == "image") return MediaKind::Image;
    return MediaKind::Unknown;
}

constexpr int kSchema = 1;

}  // namespace

bool MediaPool::add(const PooledItem& item) {
    if (item.path.empty()) {
        return false;
    }
    const auto at = std::find_if(items_.begin(), items_.end(),
                                 [&item](const PooledItem& existing) {
                                     return existing.path == item.path;
                                 });
    if (at != items_.end()) {
        // Already pooled; re-import must not overwrite firstSeen.
        return false;
    }
    items_.push_back(item);
    return true;
}

void MediaPool::load(const std::string& file) {
    items_.clear();

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return;  // no pool yet is the normal state on a fresh install
    }

    json doc;
    try {
        in >> doc;
    } catch (...) {
        return;  // a corrupt pool is an empty pool, never a failed launch
    }
    if (!doc.is_object() || !doc.contains("items") || !doc["items"].is_array()) {
        return;
    }
    if (doc.value("schema", 0) > kSchema) {
        return;  // written by a newer Ruby; do not guess at it
    }

    for (const json& entry : doc["items"]) {
        if (!entry.is_object()) {
            continue;
        }
        PooledItem item;
        item.path = entry.value("path", std::string());
        if (item.path.empty()) {
            continue;
        }
        item.name = entry.value("name", std::string());
        item.kind = mediaKind(entry.value("kind", std::string("unknown")));
        item.duration = entry.value("duration", 0.0);
        item.bytes = entry.value("bytes", std::int64_t{0});
        item.firstSeen = entry.value("firstSeen", std::int64_t{0});
        add(item);
    }
}

bool MediaPool::save(const std::string& file) const {
    json doc;
    doc["schema"] = kSchema;

    json entries = json::array();
    for (const PooledItem& item : items_) {
        json entry;
        entry["path"] = item.path;
        entry["name"] = item.name;
        entry["kind"] = name(item.kind);
        entry["duration"] = item.duration;
        entry["bytes"] = item.bytes;
        entry["firstSeen"] = item.firstSeen;
        entries.push_back(std::move(entry));
    }
    doc["items"] = std::move(entries);

    // Temp file then rename, as the project format does, so a mid-save quit can't
    // corrupt the whole pool.
    const std::string temp = file + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << doc.dump(2) << '\n';
        if (!out) {
            return false;
        }
    }
    if (std::rename(temp.c_str(), file.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

}  // namespace ruby::io
