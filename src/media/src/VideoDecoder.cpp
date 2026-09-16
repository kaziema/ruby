#include "ruby/media/VideoDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>

namespace ruby::media {

class VideoDecoder::Impl {
public:
    ~Impl() {
        if (scaler != nullptr) {
            sws_freeContext(scaler);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scaler = nullptr;

    int stream = -1;
    AVRational timeBase{1, 1};
    double duration = 0.0;
    double fps = 0.0;

    VideoFrame current;
    double currentTime = -1.0;
    bool positioned = false;

    [[nodiscard]] double toSeconds(std::int64_t pts) const {
        if (pts == AV_NOPTS_VALUE) {
            return 0.0;
        }
        return static_cast<double>(pts) * av_q2d(timeBase);
    }

    void convert() {
        const int w = codec->width;
        const int h = codec->height;

        scaler = sws_getCachedContext(scaler, w, h, codec->pix_fmt, w, h, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler == nullptr) {
            return;
        }

        current.width = w;
        current.height = h;
        current.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);

        std::uint8_t* dst[4] = {current.rgba.data(), nullptr, nullptr, nullptr};
        int stride[4] = {w * 4, 0, 0, 0};
        sws_scale(scaler, frame->data, frame->linesize, 0, h, dst, stride);

        current.pts = toSeconds(frame->best_effort_timestamp);
    }

    // Pulls frames until one lands at or after `target`. Returns false at end of stream.
    bool decodeUntil(double target) {
        while (true) {
            const int got = avcodec_receive_frame(codec, frame);
            if (got == 0) {
                const double pts = toSeconds(frame->best_effort_timestamp);
                convert();
                currentTime = pts;
                if (pts >= target - 1e-6) {
                    return true;
                }
                continue;  // still behind the playhead, keep going
            }
            if (got != AVERROR(EAGAIN) && got != AVERROR_EOF) {
                return false;
            }
            if (got == AVERROR_EOF) {
                return current.valid();
            }

            // Need more input.
            av_packet_unref(packet);
            const int read = av_read_frame(format, packet);
            if (read < 0) {
                avcodec_send_packet(codec, nullptr);  // flush
                continue;
            }
            if (packet->stream_index != stream) {
                continue;
            }
            if (avcodec_send_packet(codec, packet) < 0) {
                return false;
            }
        }
    }

    void seekTo(double seconds) {
        const auto target =
            static_cast<std::int64_t>(seconds / av_q2d(timeBase));
        av_seek_frame(format, stream, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
        currentTime = -1.0;
    }
};

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(const std::string& path) {
    // FFmpeg logs benign warnings that read like failures; we report real failures via
    // null returns instead.
    static const bool quieted = [] {
        av_log_set_level(AV_LOG_FATAL);
        return true;
    }();
    (void)quieted;

    auto impl = std::make_unique<Impl>();

    if (avformat_open_input(&impl->format, path.c_str(), nullptr, nullptr) < 0) {
        return nullptr;
    }
    if (avformat_find_stream_info(impl->format, nullptr) < 0) {
        return nullptr;
    }

    const AVCodec* decoder = nullptr;
    impl->stream =
        av_find_best_stream(impl->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (impl->stream < 0 || decoder == nullptr) {
        return nullptr;
    }

    AVStream* stream = impl->format->streams[impl->stream];
    impl->timeBase = stream->time_base;
    impl->fps = av_q2d(stream->avg_frame_rate);
    impl->duration = (stream->duration > 0)
                         ? static_cast<double>(stream->duration) * av_q2d(stream->time_base)
                         : static_cast<double>(impl->format->duration) / AV_TIME_BASE;

    impl->codec = avcodec_alloc_context3(decoder);
    if (impl->codec == nullptr) {
        return nullptr;
    }
    if (avcodec_parameters_to_context(impl->codec, stream->codecpar) < 0) {
        return nullptr;
    }
    // Multithreaded decode matters: single-threaded 1080p can't keep up with scrubbing.
    impl->codec->thread_count = 0;
    if (avcodec_open2(impl->codec, decoder, nullptr) < 0) {
        return nullptr;
    }

    impl->packet = av_packet_alloc();
    impl->frame = av_frame_alloc();
    if (impl->packet == nullptr || impl->frame == nullptr) {
        return nullptr;
    }

    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

int VideoDecoder::width() const noexcept { return impl_->codec->width; }
int VideoDecoder::height() const noexcept { return impl_->codec->height; }
double VideoDecoder::duration() const noexcept { return impl_->duration; }
double VideoDecoder::fps() const noexcept { return impl_->fps; }

const VideoFrame* VideoDecoder::frameAt(double seconds) {
    seconds = std::clamp(seconds, 0.0, impl_->duration);

    // Already on the right frame; common while scrubbing, cheaper to check than to seek.
    const double frameDuration = (impl_->fps > 0.0) ? 1.0 / impl_->fps : 1.0 / 30.0;
    if (impl_->positioned && impl_->current.valid() && impl_->currentTime >= 0.0 &&
        seconds >= impl_->currentTime && seconds < impl_->currentTime + frameDuration) {
        return &impl_->current;
    }

    // Seek unless moving forward by less than a second, where decoding through is faster.
    const bool forwardNearby = impl_->positioned && impl_->currentTime >= 0.0 &&
                               seconds > impl_->currentTime &&
                               seconds - impl_->currentTime < 1.0;
    if (!forwardNearby) {
        impl_->seekTo(seconds);
    }

    if (!impl_->decodeUntil(seconds)) {
        return impl_->current.valid() ? &impl_->current : nullptr;
    }
    impl_->positioned = true;
    return &impl_->current;
}

}  // namespace ruby::media
