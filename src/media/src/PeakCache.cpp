#include "ruby/media/PeakCache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ruby::media {
namespace {

// Below this, structure isn't visible; not worth keeping another level.
constexpr std::size_t kSmallestLevel = 64;

constexpr char kMagic[8] = {'R', 'B', 'Y', 'P', 'E', 'A', 'K', '1'};

template <typename T>
void write(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool read(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

}  // namespace

PeakPyramid PeakPyramid::build(const AudioBuffer& buffer) {
    PeakPyramid pyramid;
    if (!buffer.valid()) {
        return pyramid;
    }
    pyramid.duration_ = buffer.duration();

    // Base level, straight off the samples.
    const WaveformPeaks base = AudioDecoder::peaks(buffer, kBasePeaksPerSecond);
    if (base.empty()) {
        return pyramid;
    }
    PeakLevel first;
    first.bucketsPerSecond = base.bucketsPerSecond;
    first.low = base.low;
    first.high = base.high;
    pyramid.levels_.push_back(std::move(first));

    // Each level built from the one above, not the samples — exact for min/max, and
    // keeps the sample pass to once.
    while (pyramid.levels_.back().count() > kSmallestLevel) {
        const PeakLevel& above = pyramid.levels_.back();
        const std::size_t count =
            (above.count() + kPeakLevelRatio - 1) / static_cast<std::size_t>(kPeakLevelRatio);

        PeakLevel level;
        level.bucketsPerSecond = above.bucketsPerSecond / kPeakLevelRatio;
        level.low.resize(count);
        level.high.resize(count);

        for (std::size_t b = 0; b < count; ++b) {
            const std::size_t begin = b * static_cast<std::size_t>(kPeakLevelRatio);
            const std::size_t end =
                std::min(begin + static_cast<std::size_t>(kPeakLevelRatio), above.count());
            float lo = above.low[begin];
            float hi = above.high[begin];
            for (std::size_t i = begin + 1; i < end; ++i) {
                lo = std::min(lo, above.low[i]);
                hi = std::max(hi, above.high[i]);
            }
            level.low[b] = lo;
            level.high[b] = hi;
        }
        pyramid.levels_.push_back(std::move(level));
    }
    return pyramid;
}

const PeakLevel* PeakPyramid::levelFor(double secondsPerPixel) const {
    if (levels_.empty()) {
        return nullptr;
    }
    if (secondsPerPixel <= 0.0) {
        return &levels_.front();  // finest we have
    }
    const double pixelsPerSecond = 1.0 / secondsPerPixel;

    // Levels run fine to coarse; pick the coarsest that still gives >= 1 bucket/pixel.
    const PeakLevel* best = &levels_.front();
    for (const PeakLevel& level : levels_) {
        if (level.bucketsPerSecond < pixelsPerSecond) {
            break;
        }
        best = &level;
    }
    return best;
}

bool PeakPyramid::save(const std::string& file) const {
    if (levels_.empty()) {
        return false;
    }
    const std::string temp = file + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(kMagic, sizeof(kMagic));
        write(out, duration_);
        write(out, static_cast<std::uint32_t>(levels_.size()));
        for (const PeakLevel& level : levels_) {
            write(out, level.bucketsPerSecond);
            write(out, static_cast<std::uint64_t>(level.count()));
            out.write(reinterpret_cast<const char*>(level.low.data()),
                      static_cast<std::streamsize>(level.low.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(level.high.data()),
                      static_cast<std::streamsize>(level.high.size() * sizeof(float)));
        }
        if (!out) {
            return false;
        }
    }
    // Write to temp then rename, so a half-written cache is never read as complete.
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool PeakPyramid::load(const std::string& file) {
    levels_.clear();
    duration_ = 0.0;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    char magic[sizeof(kMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return false;  // not ours, or a version we do not read
    }
    if (!read(in, duration_)) {
        return false;
    }
    std::uint32_t levelCount = 0;
    if (!read(in, levelCount) || levelCount == 0 || levelCount > 64) {
        return false;
    }

    for (std::uint32_t i = 0; i < levelCount; ++i) {
        PeakLevel level;
        std::uint64_t count = 0;
        if (!read(in, level.bucketsPerSecond) || !read(in, count)) {
            levels_.clear();
            return false;
        }
        // Cap allocation size before trusting a possibly truncated/hostile file.
        if (count == 0 || count > (1ULL << 32)) {
            levels_.clear();
            return false;
        }
        level.low.resize(static_cast<std::size_t>(count));
        level.high.resize(static_cast<std::size_t>(count));
        in.read(reinterpret_cast<char*>(level.low.data()),
                static_cast<std::streamsize>(level.low.size() * sizeof(float)));
        in.read(reinterpret_cast<char*>(level.high.data()),
                static_cast<std::streamsize>(level.high.size() * sizeof(float)));
        if (!in) {
            levels_.clear();
            return false;
        }
        levels_.push_back(std::move(level));
    }
    return true;
}

std::string peakCachePath(const std::string& directory, const std::string& mediaPath) {
    // Path+size+mtime: path alone would serve stale peaks after a same-name re-export.
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(mediaPath, ec);
    const auto written = std::filesystem::last_write_time(mediaPath, ec);
    const auto stamp =
        static_cast<std::uint64_t>(written.time_since_epoch().count());

    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    for (const char c : mediaPath) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    mix(static_cast<std::uint64_t>(size));
    mix(stamp);

    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.rbypeak",
                  static_cast<unsigned long long>(hash));
    return directory + "/" + name;
}

}  // namespace ruby::media
