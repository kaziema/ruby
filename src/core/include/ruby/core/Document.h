#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruby/core/Animation.h"
#include "ruby/core/Units.h"

namespace ruby::core {

using LayerId = std::uint64_t;
using CompId = std::uint64_t;
using MediaId = std::uint64_t;

// --- Media pool --------------------------------------------------------------
//
// Imported files live here once; layers reference them by id, so moving a file or
// sharing a decoder is a single fix, not a hunt through every layer.

enum class MediaKind {
    Video,   // has picture, may also have sound
    Audio,   // sound only
    Image,
    Unknown,
};

struct MediaItem {
    MediaId id = 0;
    std::string path;  // absolute
    std::string name;  // file name, what the project panel shows
    MediaKind kind = MediaKind::Unknown;

    double duration = 0.0;  // seconds
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hasAudio = false;

    [[nodiscard]] bool isVideo() const noexcept { return kind == MediaKind::Video; }
};

// --- Rhythm map --------------------------------------------------------------
//
// The track's rhythm is a document object, not user-placed markers. Two lanes: beats
// are a periodic grid to quantize against, vocal onsets are an aperiodic list where
// "snap to the half beat" doesn't apply.
//
// Detection lives in the private `beat` module; this type is public. An empty map is
// an ordinary legal state — every query degrades to a no-op rather than failing.

enum class MarkerLane {
    Beat,      // an ordinary beat
    Downbeat,  // beat one of a bar
    Vocal,     // a syllable onset
    User,      // placed or moved by hand
};

struct Marker {
    double seconds = 0.0;
    MarkerLane lane = MarkerLane::Beat;
    float strength = 1.0f;  // onset strength or detector confidence, 0..1
    int index = 0;          // running count within its lane
};

class RhythmMap {
public:
    RhythmMap() = default;

    // Replaces one lane; user markers are never touched, so re-analysis can't discard
    // a manual correction.
    void setLane(MarkerLane lane, std::vector<Marker> markers);
    void clearLane(MarkerLane lane);

    void addUserMarker(double seconds);
    bool removeMarkerNear(double seconds, double tolerance);

    void setTempo(double bpm) noexcept { bpm_ = bpm; }
    [[nodiscard]] double bpm() const noexcept { return bpm_; }

    [[nodiscard]] bool empty() const noexcept { return markers_.empty(); }
    [[nodiscard]] const std::vector<Marker>& markers() const noexcept { return markers_; }
    [[nodiscard]] bool has(MarkerLane lane) const noexcept;

    // nullopt only when there is nothing to return.
    [[nodiscard]] std::optional<Marker> nearest(double seconds) const;
    [[nodiscard]] std::optional<Marker> nearestIn(double seconds,
                                                  std::initializer_list<MarkerLane> lanes)
        const;

    // Returns input unchanged if nothing to snap to, so callers never special-case
    // an unanalyzed project.
    [[nodiscard]] double snap(double seconds) const;
    [[nodiscard]] double snapTo(double seconds,
                                std::initializer_list<MarkerLane> lanes) const;

    // Cut points in [from, to).
    [[nodiscard]] std::vector<double> between(double from, double to,
                                              std::initializer_list<MarkerLane> lanes) const;

private:
    void resort();

    std::vector<Marker> markers_;
    double bpm_ = 0.0;
};

// --- Layers ------------------------------------------------------------------

enum class LayerKind {
    Footage,
    Precomp,
    Text,
    Shape,
    Solid,
    Adjustment,
    Null,
    Audio,
};

// Horizontal alignment of a text layer's lines against each other.
enum class TextAlign {
    Left,
    Center,
    Right,
};

enum class BlendMode {
    Normal,
    Add,
    Screen,
    Multiply,
    Overlay,
    SoftLight,
    HardLight,
    Difference,
    Lighten,
    Darken,
};

// Enum so the UI owns the actual hex values.
enum class LabelColor {
    Lavender,
    Aqua,
    Gray,
    Green,
};

[[nodiscard]] LabelColor defaultLabelFor(LayerKind kind) noexcept;

// One effect applied to a layer. Parameters are Properties, so every control animates
// for free. `effectId`+`schema` are the identity pair driving migrations.
struct EffectInstance {
    std::string effectId;
    int schema = 1;
    std::string displayName;  // cached from the registry for the inspector
    bool enabled = true;
    bool expanded = true;  // its parameters showing under it in the timeline
    std::vector<Property> params;

    [[nodiscard]] Property* find(std::string_view key) noexcept;
    [[nodiscard]] const Property* find(std::string_view key) const noexcept;
};

// Min/max per time bucket, precomputed for drawing — avoids touching millions of raw
// samples per repaint, and keeps the document decoder-agnostic.
struct Waveform {
    double bucketsPerSecond = 0.0;
    std::vector<float> low;
    std::vector<float> high;

    [[nodiscard]] bool empty() const noexcept { return low.empty(); }
};

struct Layer {
    LayerId id = 0;
    std::string name;
    LayerKind kind = LayerKind::Footage;
    LabelColor label = LabelColor::Gray;

    TimeValue inPoint = TimeValue::seconds(0.0);
    TimeValue outPoint = TimeValue::seconds(0.0);

    BlendMode blend = BlendMode::Normal;
    std::optional<LayerId> parent;
    std::optional<CompId> source;  // set on Precomp layers
    std::optional<MediaId> media;  // resolved through the project's pool

    bool enabled = true;       // the eye: whether the layer is drawn
    bool audioEnabled = true;  // the speaker: whether it is heard
    bool solo = false;

    // Blocks anything that changes the layer (select, move, trim, blend, parent,
    // keyframes). Visibility/audio/solo/expand stay live — locking is often to keep
    // watching a layer while working around it.
    bool locked = false;

    // Text layers only. Structured fields, not an HTML blob, so the format doesn't
    // depend on a specific Qt version's HTML subset. One style for the whole layer in
    // v1; per-run styling builds on top later rather than replacing this.
    std::string text;
    std::string fontFamily = "Helvetica";
    double fontSize = 72.0;      // points at 96 DPI, fixed so a size means one thing
    double tracking = 0.0;       // extra advance per character, in points
    double lineHeight = 1.2;     // multiple of the font's natural line spacing
    Value textColor = Value::rgba(1.0, 1.0, 1.0, 1.0);
    Value strokeColor = Value::rgba(0.0, 0.0, 0.0, 1.0);
    double strokeWidth = 0.0;    // 0 disables the stroke entirely
    TextAlign textAlign = TextAlign::Center;

    // Solid layers only. Carries its own size (solids are often bars/cards/letterbox
    // bands). Zero means match the composition, so resizing later doesn't freeze old
    // solids at their creation size.
    Value solidColor = Value::rgba(0.5, 0.5, 0.5, 1.0);
    int solidWidth = 0;
    int solidHeight = 0;
    bool expanded = false;  // twirled open in the timeline

    // Whether the Transform group is open, separate from `expanded` (layer twirl vs.
    // this specific group). Defaults open so pre-groups projects look unchanged.
    bool transformExpanded = true;

    std::vector<Property> properties;
    std::vector<EffectInstance> effects;  // applied in order, top to bottom
    Waveform waveform;                    // audio layers only

    [[nodiscard]] Property* find(std::string_view key) noexcept;
    [[nodiscard]] const Property* find(std::string_view key) const noexcept;

    [[nodiscard]] int keyframeCount() const noexcept;
};

// The transform stack every layer gets, matching the design's inspector.
[[nodiscard]] std::vector<Property> defaultTransform();

// Re-applies the parts of a property owned by its definition rather than the document
// (currently: range). Ranges aren't persisted — the file stores what the user chose,
// the app supplies what the parameter currently is, so widening a range later reaches
// old projects too. Called on load; unmatched properties are left alone.
void adoptTransformRanges(Layer& layer);

// Adds the "audio_level" property (key `kAudioLevelKey`) if the layer doesn't already
// have one. Stored as linear gain, 1.0 = unity/0dB; the inspector displays it in dB via
// SpatialUnit::Decibels. Called by addLayer() for new layers and on load for old ones,
// same reason as adoptTransformRanges: layers saved before this existed must still work.
inline constexpr const char* kAudioLevelKey = "audio_level";
void ensureAudioLevel(Layer& layer);

// --- Composition -------------------------------------------------------------

struct Composition {
    CompId id = 0;
    std::string name;

    int width = 1080;
    int height = 1920;  // vertical by default; this is a short-form tool
    double fps = 30.0;
    double duration = 12.0;  // seconds

    std::vector<Layer> layers;  // index 0 is the topmost layer, as in AE
    RhythmMap rhythm;

    // The part being worked on and the part being delivered — one range, not two,
    // since they're usually the same (bounds both the preview cache and export).
    // Zero width means the whole composition, so a fresh comp works without dragging
    // brackets first.
    TimeValue workIn = TimeValue::seconds(0.0);
    TimeValue workOut = TimeValue::seconds(0.0);

    [[nodiscard]] bool hasWorkArea() const noexcept;

    // Work area if set, else the whole composition — "no work area" and "full-range
    // work area" must behave identically.
    void workRange(double& startSeconds, double& endSeconds) const noexcept;

    [[nodiscard]] TimeContext timeContext() const noexcept;
    [[nodiscard]] FrameGeometry geometry() const noexcept { return {width, height}; }

    [[nodiscard]] Layer* find(LayerId layer) noexcept;
    [[nodiscard]] const Layer* find(LayerId layer) const noexcept;

    [[nodiscard]] int totalKeyframes() const noexcept;

    // Ids only need to be unique within this composition.
    [[nodiscard]] LayerId nextLayerId() const noexcept;

    // Removes a layer and clears any parent link pointing at it — a dangling parent id
    // bites later when something walks the chain. Returns false if the id wasn't found.
    bool removeLayer(LayerId layer) noexcept;

    // The time the last layer stops. 0.0 for an empty composition.
    [[nodiscard]] double contentEnd() const noexcept;

    // Grows the composition to fit its layers. Returns true if duration changed.
    // Only ever grows — shrinking hides content and must stay a deliberate act in
    // Composition Settings; it would also invalidate trims made on this comp elsewhere.
    bool growToFit() noexcept;
};

// --- Project -----------------------------------------------------------------

class Project {
public:
    Composition& addComposition(std::string name, int w, int h, double framesPerSecond,
                                double durationSeconds);

    Layer& addLayer(Composition& comp, std::string name, LayerKind kind);

    // Adds an already-probed file. Re-importing the same path returns the existing
    // entry rather than duplicating it.
    MediaItem& addMedia(std::string path, std::string name, MediaKind kind,
                        double duration, int width, int height, double fps,
                        bool hasAudio);

    [[nodiscard]] const std::vector<MediaItem>& media() const noexcept { return media_; }

    // Removes a media item and clears every layer referencing it (a dangling media id
    // is worse than none). Returns affected layer count for a confirmation prompt.
    std::size_t removeMedia(MediaId media);

    // How many layers, across every composition, use this item (for a delete confirmation).
    [[nodiscard]] std::size_t usageCount(MediaId media) const noexcept;
    [[nodiscard]] const MediaItem* findMedia(MediaId id) const noexcept;
    [[nodiscard]] const MediaItem* findMediaByPath(std::string_view path) const noexcept;

    // Convenience for the compositor and decoders: the file behind a layer, or empty.
    [[nodiscard]] std::string pathFor(const Layer& layer) const;

    [[nodiscard]] std::vector<Composition>& compositions() noexcept { return comps_; }
    [[nodiscard]] const std::vector<Composition>& compositions() const noexcept {
        return comps_;
    }

    [[nodiscard]] Composition* find(CompId comp) noexcept;

    // Loading restores ids from file rather than minting new; tells the generator
    // what's taken so new ids don't collide.
    void noteUsedId(std::uint64_t id) noexcept;

private:
    std::vector<Composition> comps_;
    std::vector<MediaItem> media_;
    std::uint64_t nextId_ = 1;
};

}  // namespace ruby::core
