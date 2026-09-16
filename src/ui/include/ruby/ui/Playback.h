#pragma once

#include <QElapsedTimer>
#include <QObject>

#include "ruby/audio/AudioOutput.h"
#include "ruby/engine/Transport.h"

class QTimer;

namespace ruby::ui {

// Drives the transport from Qt's event loop. Ticks faster than the frame rate since
// Qt timers aren't precise enough to hit exact frame boundaries.
class Playback : public QObject {
    Q_OBJECT

public:
    explicit Playback(QObject* parent = nullptr);

    void configure(double duration, double frameRate);

    // Attaching audio makes the device the clock; null falls back to the wall clock.
    void setAudio(audio::AudioOutput* output);

    [[nodiscard]] bool playing() const noexcept { return transport_.playing(); }
    [[nodiscard]] double time() const noexcept { return transport_.time(); }

    // Frames actually delivered per second, smoothed. The honest number, not the target.
    [[nodiscard]] double measuredFps() const noexcept { return measuredFps_; }

public slots:
    void togglePlay();
    void stop();
    void seek(double seconds);

signals:
    void timeChanged(double seconds);
    void playingChanged(bool playing);

private:
    void tick();

    engine::Transport transport_;
    audio::AudioOutput* audio_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_;
    double measuredFps_ = 0.0;
    qint64 lastElapsedNs_ = 0;  // for the wall-clock delta
    qint64 lastFrameNs_ = 0;    // for the measured frame rate
};

}  // namespace ruby::ui
