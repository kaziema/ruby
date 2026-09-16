// Layers reference imported files by id, not path, so two layers on one clip share a source.

#include <cstdio>
#include <cstdlib>

#include "ruby/core/Document.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

MediaItem& importClip(Project& p, const char* path, const char* name) {
    return p.addMedia(path, name, MediaKind::Video, 12.0, 1920, 1080, 30.0, true);
}

}  // namespace

int main() {
    Project project;

    MediaItem& first = importClip(project, "/clips/a.mp4", "a.mp4");
    check(first.id != 0, "an imported item gets an id");
    check(project.media().size() == 1, "and lands in the pool");

    // Re-importing the same path must be a no-op, not a duplicate entry.
    const MediaId firstId = first.id;
    MediaItem& again = importClip(project, "/clips/a.mp4", "a.mp4");
    check(again.id == firstId, "re-importing the same path returns the same item");
    check(project.media().size() == 1, "and does not duplicate it");

    importClip(project, "/clips/b.mp4", "b.mp4");
    check(project.media().size() == 2, "a different path is a different item");

    check(project.findMedia(firstId) != nullptr, "items are findable by id");
    check(project.findMedia(99999) == nullptr, "an unknown id returns nothing");
    check(project.findMediaByPath("/clips/b.mp4") != nullptr, "items are findable by path");
    check(project.findMediaByPath("/clips/nope.mp4") == nullptr,
          "an unknown path returns nothing");

    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    Layer& layer = project.addLayer(comp, "a.mp4", LayerKind::Footage);

    check(project.pathFor(layer).empty(), "an unlinked layer has no file");

    layer.media = firstId;
    check(project.pathFor(comp.layers.front()) == "/clips/a.mp4",
          "a linked layer resolves through the pool");

    // A dangling media reference must fail quietly, not resolve to a garbage path.
    comp.layers.front().media = 4242;
    check(project.pathFor(comp.layers.front()).empty(),
          "a reference to a missing item resolves to nothing");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("project: all checks passed");
    return EXIT_SUCCESS;
}
