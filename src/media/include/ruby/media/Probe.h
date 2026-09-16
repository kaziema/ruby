#pragma once

#include <optional>
#include <string>

namespace ruby::media {

// File metadata read from container/stream headers only, no decoding — stays fast
// regardless of file size.
struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;

    int width = 0;
    int height = 0;
    double fps = 0.0;

    int sampleRate = 0;
    int channels = 0;

    double duration = 0.0;  // seconds, longest stream

    [[nodiscard]] bool valid() const noexcept { return hasVideo || hasAudio; }
};

// Nullopt when the file cannot be opened or holds nothing we can use.
[[nodiscard]] std::optional<MediaInfo> probe(const std::string& path);

}  // namespace ruby::media
