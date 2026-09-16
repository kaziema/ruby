#include "ruby/ui/Playback.h"

#include <QTimer>
#include <algorithm>

namespace ruby::ui {

Playback::Playback(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &Playback::tick);
}

void Playback::configure(double duration, double frameRate) {
    transport_.setDuration(duration);
    transport_.setFrameRate(frameRate);

    // Wake ~4x per frame; the transport corrects for whatever the real interval was.
    const double frameMs = 1000.0 / transport_.frameRate();
    timer_->setInterval(std::max(1, static_cast<int>(frameMs / 4.0)));
}

void Playback::setAudio(audio::AudioOutput* output) { audio_ = output; }

void Playback::togglePlay() {
    transport_.toggle();
    if (transport_.playing()) {
        clock_.restart();
        lastElapsedNs_ = 0;
        lastFrameNs_ = 0;
        if (audio_ != nullptr) {
            audio_->play(transport_.time());
        }
        timer_->start();
    } else {
        if (audio_ != nullptr) {
            audio_->stop();
        }
        timer_->stop();
        measuredFps_ = 0.0;
    }
    emit playingChanged(transport_.playing());
}

void Playback::stop() {
    if (!transport_.playing()) {
        return;
    }
    togglePlay();
}

void Playback::seek(double seconds) {
    transport_.setTime(seconds);
    if (audio_ != nullptr && transport_.playing()) {
        audio_->play(transport_.time());  // re-cue the device to the new position
    }
    emit timeChanged(transport_.time());
}

void Playback::tick() {
    const qint64 nowNs = clock_.nsecsElapsed();
    const int before = transport_.frame();

    // Audio device is the clock when playing: its crystal won't match the system clock,
    // and wall-clock timing would let picture and sound drift apart.
    const bool audioDriving = audio_ != nullptr && audio_->playing();
    if (audioDriving) {
        // Detect end-of-composition here rather than end-of-clip: silence between layers
        // would otherwise stop playback mid-composition.
        const double position = audio_->position();
        if (transport_.duration() > 0.0 && position >= transport_.duration()) {
            if (transport_.looping()) {
                transport_.setTime(0.0);
                audio_->play(0.0);
            } else {
                transport_.stop();
                audio_->stop();
            }
        } else {
            transport_.setTime(position);
        }
    } else {
        const double elapsed = static_cast<double>(nowNs - lastElapsedNs_) / 1e9;
        transport_.advance(elapsed);  // wraps by remainder on its own
    }
    lastElapsedNs_ = nowNs;

    if (transport_.frame() == before && transport_.playing()) {
        return;  // still inside the same frame; nothing to redraw
    }

    if (lastFrameNs_ != 0) {
        const double delta = static_cast<double>(nowNs - lastFrameNs_) / 1e9;
        if (delta > 0.0) {
            // Exponential smoothing: the raw number jitters too much to read.
            const double instant = 1.0 / delta;
            measuredFps_ = (measuredFps_ <= 0.0) ? instant
                                                 : measuredFps_ * 0.9 + instant * 0.1;
        }
    }
    lastFrameNs_ = nowNs;

    emit timeChanged(transport_.time());

    if (!transport_.playing()) {
        timer_->stop();
        measuredFps_ = 0.0;
        emit playingChanged(false);
    }
}

}  // namespace ruby::ui
