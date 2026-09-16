#include "ruby/media/Probe.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <algorithm>

namespace ruby::media {

std::optional<MediaInfo> probe(const std::string& path) {
    static const bool quieted = [] {
        av_log_set_level(AV_LOG_FATAL);
        return true;
    }();
    (void)quieted;

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    struct Guard {
        AVFormatContext** f;
        ~Guard() { avformat_close_input(f); }
    } guard{&format};

    if (avformat_find_stream_info(format, nullptr) < 0) {
        return std::nullopt;
    }

    MediaInfo info;
    if (format->duration > 0) {
        info.duration = static_cast<double>(format->duration) / AV_TIME_BASE;
    }

    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVStream* stream = format->streams[i];
        const AVCodecParameters* codec = stream->codecpar;

        const double streamDuration =
            (stream->duration > 0)
                ? static_cast<double>(stream->duration) * av_q2d(stream->time_base)
                : 0.0;
        info.duration = std::max(info.duration, streamDuration);

        if (codec->codec_type == AVMEDIA_TYPE_VIDEO && !info.hasVideo) {
            // Attached cover art is a video stream but not footage; skip it.
            if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
                continue;
            }
            info.hasVideo = true;
            info.width = codec->width;
            info.height = codec->height;
            info.fps = av_q2d(stream->avg_frame_rate);
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO && !info.hasAudio) {
            info.hasAudio = true;
            info.sampleRate = codec->sample_rate;
            info.channels = codec->ch_layout.nb_channels;
        }
    }

    return info.valid() ? std::optional<MediaInfo>(info) : std::nullopt;
}

}  // namespace ruby::media
