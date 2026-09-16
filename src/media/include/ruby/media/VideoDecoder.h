#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ruby::media {

// One decoded frame, converted to tightly packed RGBA8 (downstream wants a texture,
// not source YUV). CPU conversion for now; GPU would be faster, later.
struct VideoFrame {
    int width = 0;
    int height = 0;
    double pts = 0.0;  // presentation time in seconds
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && !rgba.empty();
    }
};

// Decodes video frames at arbitrary times.
//
// Seeking is naive: jumps to the keyframe at/before target and decodes forward. Correct
// but slow to scrub on long-GOP footage; wants a real keyframe index eventually.
class VideoDecoder {
public:
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Returns null if the file cannot be opened or has no video stream.
    [[nodiscard]] static std::unique_ptr<VideoDecoder> open(const std::string& path);

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] double duration() const noexcept;  // seconds
    [[nodiscard]] double fps() const noexcept;

    // The frame at or just before `seconds`. Null if decoding failed. The returned
    // pointer belongs to the decoder and is valid until the next call.
    [[nodiscard]] const VideoFrame* frameAt(double seconds);

private:
    class Impl;
    explicit VideoDecoder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace ruby::media
