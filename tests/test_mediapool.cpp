// Tests for the app-level media pool: dedup, round-trip, and refusing to fail loudly.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "ruby/io/MediaPool.h"

using namespace ruby::io;
using ruby::core::MediaKind;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string tempFile(const char* leaf) {
    return (std::filesystem::temp_directory_path() / leaf).string();
}

PooledItem clip(const char* path, std::int64_t firstSeen) {
    PooledItem item;
    item.path = path;
    item.name = "clip.mov";
    item.kind = MediaKind::Video;
    item.duration = 9.79;
    item.bytes = 1234;
    item.firstSeen = firstSeen;
    return item;
}

void the_same_path_is_pooled_once() {
    MediaPool pool;
    check(pool.empty(), "a fresh pool is empty");

    check(pool.add(clip("/a/clip.mov", 1000)), "first import is new");
    check(!pool.add(clip("/a/clip.mov", 2000)), "the same path again is not new");
    check(pool.items().size() == 1, "and did not add a second entry");

    check(pool.items().front().firstSeen == 1000, "the original first-seen time survived");

    check(!pool.add(clip("", 3000)), "an empty path is refused");
    check(pool.items().size() == 1, "and adds nothing");
}

void a_pool_survives_a_round_trip() {
    const std::string file = tempFile("ruby_pool_roundtrip.json");
    std::filesystem::remove(file);

    MediaPool written;
    written.add(clip("/a/one.mov", 111));
    PooledItem song;
    song.path = "/a/two.wav";
    song.name = "two.wav";
    song.kind = MediaKind::Audio;
    song.duration = 180.5;
    song.bytes = 999;
    song.firstSeen = 222;
    written.add(song);

    check(written.save(file), "saved");

    MediaPool read;
    read.load(file);
    check(read.items().size() == 2, "both entries came back");
    check(read.items()[0].path == "/a/one.mov", "order preserved");
    check(read.items()[1].kind == MediaKind::Audio, "kind survived as a name");
    check(read.items()[1].duration > 180.4 && read.items()[1].duration < 180.6,
          "duration survived");
    check(read.items()[1].firstSeen == 222, "first-seen survived");

    std::filesystem::remove(file);
}

void a_broken_pool_is_an_empty_pool() {
    MediaPool missing;
    missing.load(tempFile("ruby_pool_does_not_exist.json"));
    check(missing.empty(), "a pool file that is not there loads as empty");

    const std::string file = tempFile("ruby_pool_corrupt.json");
    {
        std::ofstream out(file, std::ios::trunc);
        out << "{ this is not json at all ";
    }
    MediaPool corrupt;
    corrupt.add(clip("/a/stale.mov", 1));
    corrupt.load(file);
    check(corrupt.empty(), "a corrupt pool loads as empty rather than throwing");
    std::filesystem::remove(file);

    const std::string newer = tempFile("ruby_pool_newer.json");
    {
        std::ofstream out(newer, std::ios::trunc);
        out << R"({"schema": 99, "items": [{"path": "/a/x.mov"}]})";
    }
    MediaPool future;
    future.load(newer);
    check(future.empty(), "a pool from a newer Ruby is not guessed at");
    std::filesystem::remove(newer);
}

}  // namespace

int main() {
    the_same_path_is_pooled_once();
    a_pool_survives_a_round_trip();
    a_broken_pool_is_an_empty_pool();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("mediapool: all checks passed");
    return EXIT_SUCCESS;
}
