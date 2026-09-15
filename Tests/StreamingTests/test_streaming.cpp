// Tests/StreamingTests/test_streaming.cpp — Phase 11, W5 (world streaming seam)
//
// Two layers, tested separately because they fail for different reasons:
//
//   * The grid math (NF/Scene/StreamingVolume.hpp) is pure geometry. It is where
//     an off-by-one in a floor, or a distance measured to a chunk's centre
//     instead of its box, would quietly put a hole in the world.
//   * The policy (WorldStreamer) decides what is loaded. It is driven here with
//     a recording fake, so "did it load the right chunks" is asserted without a
//     filesystem, a GPU, or a real scene.
//
// The hysteresis tests are the ones that matter. A streamer with a single radius
// passes every naive test and then loads and unloads the same chunk on every
// frame while the player walks along a boundary — file IO and entity churn, per
// frame, forever.

#include <NF/Test/TestFramework.hpp>

#include <NF/Runtime/WorldStreamer.hpp>
#include <NF/Scene/StreamingVolume.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace nf;
using namespace nf::runtime;
using namespace nf::scene;

namespace {

/// Records what the streamer asked for, and hands back a plausible entity list so
/// `streamed_entity_count` has something to count.
class RecordingHandlers {
public:
    explicit RecordingHandlers(u32 entities_per_chunk = 3)
        : m_entities_per_chunk(entities_per_chunk) {}

    bool load(const ChunkCoord& coord, std::vector<ecs::Entity>& out_created, std::string& out_error) {
        ++load_calls;
        loaded_log.push_back(coord);

        if (fail_for.count(coord) != 0) {
            out_error = "chunk " + std::to_string(coord.x) + " is missing";
            return false;
        }

        out_created.clear();
        for (u32 i = 0; i < m_entities_per_chunk; ++i) {
            ecs::Entity e;
            // A distinct id per (chunk, index) so an unload can be checked against
            // what the load actually produced rather than against a count.
            e.id = static_cast<u32>((coord.x + 1000) * 100 + static_cast<i32>(i));
            out_created.push_back(e);
        }
        return true;
    }

    void unload(const ChunkCoord& coord, const std::vector<ecs::Entity>& created) {
        ++unload_calls;
        unloaded_log.push_back(coord);
        unloaded_entities += created.size();
    }

    void attach(WorldStreamer& streamer) {
        streamer.set_handlers(
            [this](const ChunkCoord& c, std::vector<ecs::Entity>& out, std::string& err) {
                return load(c, out, err);
            },
            [this](const ChunkCoord& c, const std::vector<ecs::Entity>& created) {
                unload(c, created);
            });
    }

    u32 load_calls = 0;
    u32 unload_calls = 0;
    size_t unloaded_entities = 0;
    std::vector<ChunkCoord> loaded_log;
    std::vector<ChunkCoord> unloaded_log;
    std::unordered_set<ChunkCoord> fail_for;

private:
    u32 m_entities_per_chunk = 3;
};

bool contains(const std::vector<ChunkCoord>& v, const ChunkCoord& c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

// ---------------------------------------------------------------------------
// Grid math
// ---------------------------------------------------------------------------

NF_TEST(streaming_chunk_at_floors_toward_negative_infinity) {
    // Truncation would map both -0.5 and +0.5 into cell 0, making a chunk
    // boundary behave asymmetrically about the origin. Floor is the only
    // choice that keeps the grid uniform.
    NF_CHECK(scene::chunk_at(Vec3{0.5f, 0.5f, 0.5f}, 10.0f) == (ChunkCoord{0, 0, 0}));
    NF_CHECK(scene::chunk_at(Vec3{-0.5f, -0.5f, -0.5f}, 10.0f) == (ChunkCoord{-1, -1, -1}));
    NF_CHECK(scene::chunk_at(Vec3{-10.0f, 0.0f, 0.0f}, 10.0f) == (ChunkCoord{-1, 0, 0}));
    NF_CHECK(scene::chunk_at(Vec3{-10.1f, 0.0f, 0.0f}, 10.0f) == (ChunkCoord{-2, 0, 0}));
    NF_CHECK(scene::chunk_at(Vec3{10.0f, 0.0f, 0.0f}, 10.0f) == (ChunkCoord{1, 0, 0}));
}

NF_TEST(streaming_chunk_center_is_the_middle_of_the_cell) {
    const Vec3 c = scene::chunk_center(ChunkCoord{2, -1, 0}, 10.0f);
    NF_CHECK_NEAR(c.x, 25.0f, 1e-4f);
    NF_CHECK_NEAR(c.y, -5.0f, 1e-4f);
    NF_CHECK_NEAR(c.z, 5.0f, 1e-4f);
}

NF_TEST(streaming_distance_to_chunk_is_the_box_distance_not_the_centre_distance) {
    // Chunk (0,0,0) with size 10 spans [0,10)^3.
    const ChunkCoord origin{0, 0, 0};

    // Inside the box: zero, regardless of how close to the corner it is.
    NF_CHECK_NEAR(scene::distance_to_chunk(Vec3{0.1f, 0.1f, 0.1f}, origin, 10.0f), 0.0f, 1e-5f);
    NF_CHECK_NEAR(scene::distance_to_chunk(Vec3{9.9f, 9.9f, 9.9f}, origin, 10.0f), 0.0f, 1e-5f);

    // Directly off one face: the gap, not the distance to the centre (which
    // would be 5 for a point at x=15).
    NF_CHECK_NEAR(scene::distance_to_chunk(Vec3{15.0f, 5.0f, 5.0f}, origin, 10.0f), 5.0f, 1e-5f);

    // Off a corner: the diagonal gap.
    NF_CHECK_NEAR(scene::distance_to_chunk(Vec3{13.0f, 14.0f, 5.0f}, origin, 10.0f), 5.0f, 1e-5f);

    // Negative side.
    NF_CHECK_NEAR(scene::distance_to_chunk(Vec3{-4.0f, 5.0f, 5.0f}, origin, 10.0f), 4.0f, 1e-5f);
}

NF_TEST(streaming_chunks_in_radius_covers_the_sphere_nearest_first) {
    std::vector<ChunkCoord> chunks;
    scene::chunks_in_radius(Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 10.0f, chunks);

    // Every returned cell must actually intersect the sphere, and the cell the
    // centre is in must be present.
    NF_CHECK(!chunks.empty());
    NF_CHECK(contains(chunks, ChunkCoord{0, 0, 0}));

    for (const ChunkCoord& c : chunks) {
        NF_CHECK(scene::distance_to_chunk(Vec3{0.0f, 0.0f, 0.0f}, c, 10.0f) <= 10.0f + 1e-4f);
    }

    // Nearest first. This is load-bearing, not cosmetic: a caller that caps loads
    // per frame is choosing what to defer, and a coordinate order would defer
    // whatever sits at high x — possibly the ground under the player.
    for (size_t i = 1; i < chunks.size(); ++i) {
        const f32 prev = scene::distance_to_chunk(Vec3{0.0f, 0.0f, 0.0f}, chunks[i - 1], 10.0f);
        const f32 cur = scene::distance_to_chunk(Vec3{0.0f, 0.0f, 0.0f}, chunks[i], 10.0f);
        NF_CHECK(prev <= cur + 1e-4f);
    }

    // A cell far outside must not appear.
    NF_CHECK(!contains(chunks, ChunkCoord{5, 0, 0}));
}

NF_TEST(streaming_chunks_in_radius_walks_the_grid_rather_than_a_fixed_neighbourhood) {
    // A larger radius must reach further. A hardcoded 3x3x3 assumption would
    // silently under-load a large volume, and the failure would look like a hole
    // in the world rather than a bug in the streamer.
    std::vector<ChunkCoord> small;
    std::vector<ChunkCoord> large;
    scene::chunks_in_radius(Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 10.0f, small);
    scene::chunks_in_radius(Vec3{0.0f, 0.0f, 0.0f}, 60.0f, 10.0f, large);

    NF_CHECK(large.size() > small.size());
    NF_CHECK(contains(large, ChunkCoord{5, 0, 0}));
    NF_CHECK(!contains(small, ChunkCoord{5, 0, 0}));
}

NF_TEST(streaming_chunks_in_radius_degenerate_inputs_are_empty_not_garbage) {
    std::vector<ChunkCoord> chunks;

    scene::chunks_in_radius(Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 0.0f, chunks);
    NF_CHECK(chunks.empty());  // zero chunk size would divide by zero

    scene::chunks_in_radius(Vec3{0.0f, 0.0f, 0.0f}, 10.0f, -5.0f, chunks);
    NF_CHECK(chunks.empty());

    // A zero radius still yields the cell the centre is in: a degenerate volume
    // should stream exactly one chunk, not none and not everything.
    scene::chunks_in_radius(Vec3{5.0f, 5.0f, 5.0f}, 0.0f, 10.0f, chunks);
    NF_CHECK_EQ(chunks.size(), static_cast<size_t>(1));
}

NF_TEST(streaming_chunk_coord_hash_distinguishes_distinct_cells) {
    // The streamer keys its loaded set by this hash. A collision here would make
    // one chunk's entities answer for another's, so the packing is checked
    // directly rather than assumed.
    std::unordered_set<size_t> hashes;
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 y = -4; y <= 4; ++y) {
            for (i32 z = -4; z <= 4; ++z) {
                hashes.insert(std::hash<ChunkCoord>{}(ChunkCoord{x, y, z}));
            }
        }
    }
    NF_CHECK_EQ(hashes.size(), static_cast<size_t>(9 * 9 * 9));

    // And the equal-coords case must agree, or a lookup would miss.
    NF_CHECK_EQ(std::hash<ChunkCoord>{}(ChunkCoord{3, -2, 1}),
                std::hash<ChunkCoord>{}(ChunkCoord{3, -2, 1}));
}

// ---------------------------------------------------------------------------
// Streamer policy
// ---------------------------------------------------------------------------

NF_TEST(streamer_does_nothing_without_a_volume) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);

    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(handlers.load_calls, 0u);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(0));
}

NF_TEST(streamer_does_nothing_without_handlers) {
    WorldStreamer streamer;
    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 10.0f;
    streamer.set_volume(volume);

    // No handler means nothing can be loaded; the streamer must report that
    // rather than inventing chunks.
    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(0));
}

NF_TEST(streamer_ignores_an_invalid_volume) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);

    scene::StreamingVolume bad;
    bad.chunk_size = 0.0f;  // would divide by zero everywhere
    streamer.set_volume(bad);

    NF_CHECK(!streamer.has_volume());
    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(handlers.load_calls, 0u);
}

NF_TEST(streamer_loads_the_chunks_in_range_and_counts_them) {
    RecordingHandlers handlers(/*entities_per_chunk=*/3);
    WorldStreamer streamer;
    handlers.attach(streamer);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 10.0f;
    volume.unload_radius = 20.0f;
    streamer.set_volume(volume);

    streamer.set_max_loads_per_update(64);
    const u32 loaded = streamer.update();

    NF_CHECK(loaded > 0);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(loaded));
    NF_CHECK(streamer.is_loaded(ChunkCoord{0, 0, 0}));
    NF_CHECK_EQ(streamer.streamed_entity_count(), static_cast<size_t>(loaded) * 3u);
    NF_CHECK_EQ(streamer.load_requests(), loaded);

    // A second update with nothing changed must not re-load anything.
    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(handlers.load_calls, loaded);
}

NF_TEST(streamer_hysteresis_keeps_a_chunk_between_the_two_radii) {
    // chunk_size 10, load 25, unload 35. Chunk (3,0,0) spans [30,40).
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    // Cap raised out of the way: these tests are about hysteresis, and a cap
    // that truncates the wanted set would make them about the cap instead.
    streamer.set_max_loads_per_update(1000);

    scene::StreamingVolume volume;
    volume.chunk_size = 10.0f;
    volume.load_radius = 25.0f;
    volume.unload_radius = 35.0f;

    // At x = 6 the chunk's distance is 24 -> inside load_radius -> loaded.
    volume.center = Vec3{6.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.update();
    NF_CHECK(streamer.is_loaded(ChunkCoord{3, 0, 0}));

    // Move to x = 4: distance 26 -> outside load_radius, inside unload_radius.
    // It must stay loaded. This is the whole point of having two radii.
    volume.center = Vec3{4.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.update();
    NF_CHECK(streamer.is_loaded(ChunkCoord{3, 0, 0}));
    NF_CHECK_EQ(handlers.unload_calls, 0u);
}

NF_TEST(streamer_releases_a_chunk_once_it_passes_the_unload_radius) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    // Cap raised out of the way: these tests are about hysteresis, and a cap
    // that truncates the wanted set would make them about the cap instead.
    streamer.set_max_loads_per_update(1000);

    scene::StreamingVolume volume;
    volume.chunk_size = 10.0f;
    volume.load_radius = 25.0f;
    volume.unload_radius = 35.0f;

    volume.center = Vec3{6.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.update();
    NF_CHECK(streamer.is_loaded(ChunkCoord{3, 0, 0}));

    // x = -10 puts the chunk at distance 40 -> beyond unload_radius -> released.
    volume.center = Vec3{-10.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.update();

    NF_CHECK(!streamer.is_loaded(ChunkCoord{3, 0, 0}));
    // The whole far side goes, not just this chunk, so the assertion is on which
    // chunk was released rather than on a count that scales with the radius.
    NF_CHECK(handlers.unload_calls > 0);
    NF_CHECK(contains(handlers.unloaded_log, ChunkCoord{3, 0, 0}));
}

NF_TEST(streamer_does_not_thrash_on_a_boundary) {
    // The regression this whole design exists to prevent: a single-radius
    // streamer loads and unloads this chunk on every frame while the player
    // walks back and forth across a boundary.
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    // Cap raised out of the way: these tests are about hysteresis, and a cap
    // that truncates the wanted set would make them about the cap instead.
    streamer.set_max_loads_per_update(1000);

    scene::StreamingVolume volume;
    volume.chunk_size = 10.0f;
    volume.load_radius = 25.0f;
    volume.unload_radius = 35.0f;

    volume.center = Vec3{6.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.update();

    const ChunkCoord boundary{3, 0, 0};
    NF_CHECK(streamer.is_loaded(boundary));

    // Oscillate inside the hysteresis band for 40 frames.
    for (int i = 0; i < 40; ++i) {
        volume.center = Vec3{(i % 2 == 0) ? 4.0f : 6.0f, 0.0f, 0.0f};
        streamer.set_volume(volume);
        streamer.update();
    }

    NF_CHECK(streamer.is_loaded(boundary));

    // Nothing inside the hysteresis band may be released. That is the property.
    NF_CHECK_EQ(handlers.unload_calls, 0u);

    // And no chunk may be loaded twice. Chunks that genuinely enter load range as
    // the volume moves are expected to load; the *same* chunk loading again is
    // the thrash this design exists to prevent.
    std::unordered_set<ChunkCoord> seen;
    for (const ChunkCoord& c : handlers.loaded_log) {
        NF_CHECK(seen.insert(c).second);
    }
}

NF_TEST(streamer_caps_loads_per_update) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 60.0f;  // many chunks
    volume.unload_radius = 70.0f;
    streamer.set_volume(volume);

    streamer.set_max_loads_per_update(2);
    NF_CHECK_EQ(streamer.update(), 2u);
    NF_CHECK_EQ(streamer.update(), 2u);
    NF_CHECK_EQ(streamer.update(), 2u);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(6));

    // The cap bounds the *work*, not the region: the wanted set is unchanged.
    NF_CHECK(streamer.wanted_chunks().size() > 6u);
}

NF_TEST(streamer_with_a_zero_cap_loads_nothing_but_still_releases) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(64);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 25.0f;
    volume.unload_radius = 35.0f;
    streamer.set_volume(volume);
    streamer.update();
    NF_CHECK(streamer.loaded_chunk_count() > 0);

    // Paused: stop taking on new work, but still give memory back.
    streamer.set_max_loads_per_update(0);
    volume.center = Vec3{1000.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(0));
    NF_CHECK(handlers.unload_calls > 0);
}

NF_TEST(streamer_retries_a_failed_load_rather_than_blacklisting_it) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(64);

    scene::StreamingVolume volume;
    volume.center = Vec3{5.0f, 5.0f, 5.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 1.0f;   // just the centre cell
    volume.unload_radius = 20.0f;
    streamer.set_volume(volume);

    handlers.fail_for.insert(ChunkCoord{0, 0, 0});
    NF_CHECK_EQ(streamer.update(), 0u);
    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(0));

    // The error is kept, so a caller can see *why* rather than only that the
    // count stayed flat.
    NF_CHECK(!streamer.last_error().empty());

    // A chunk file that appears later must be picked up. Blacklisting a failed
    // chunk would make a late-cooked asset permanently invisible.
    handlers.fail_for.clear();
    NF_CHECK_EQ(streamer.update(), 1u);
    NF_CHECK(streamer.is_loaded(ChunkCoord{0, 0, 0}));
    NF_CHECK(streamer.last_error().empty());
}

NF_TEST(streamer_clear_releases_every_loaded_chunk) {
    RecordingHandlers handlers(/*entities_per_chunk=*/2);
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(64);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    volume.load_radius = 20.0f;
    volume.unload_radius = 30.0f;
    streamer.set_volume(volume);
    streamer.update();

    const size_t loaded = streamer.loaded_chunk_count();
    NF_CHECK(loaded > 0);

    streamer.clear();

    NF_CHECK_EQ(streamer.loaded_chunk_count(), static_cast<size_t>(0));
    NF_CHECK_EQ(handlers.unload_calls, static_cast<u32>(loaded));
    // The unload handler received the same entities the load produced, not a
    // count — that is what lets a real implementation remove exactly those.
    NF_CHECK_EQ(handlers.unloaded_entities, loaded * 2u);
}

NF_TEST(streamer_treats_an_inverted_hysteresis_band_as_a_valid_one) {
    // unload_radius < load_radius would unload the chunk that was just loaded,
    // every frame, forever. Clamping up is the only sane reading.
    scene::StreamingVolume volume;
    volume.load_radius = 50.0f;
    volume.unload_radius = 10.0f;
    NF_CHECK_NEAR(volume.effective_unload_radius(), 50.0f, 1e-5f);

    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(1000);

    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 10.0f;
    streamer.set_volume(volume);
    streamer.update();

    const size_t loaded = streamer.loaded_chunk_count();
    NF_CHECK(loaded > 0);

    // Standing still must not unload anything.
    streamer.update();
    streamer.update();
    NF_CHECK_EQ(streamer.loaded_chunk_count(), loaded);
    NF_CHECK_EQ(handlers.unload_calls, 0u);
}

// ---------------------------------------------------------------------------
// Dispatch order + memory budget (Phase 12)
// ---------------------------------------------------------------------------

NF_TEST(stream_next_wanted_loads_follows_wanted_order_skipping_loaded_and_in_flight) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 32.0f;
    volume.load_radius = 40.0f;
    volume.unload_radius = 200.0f;
    streamer.set_volume(volume);

    // Nothing loaded, two already in flight: the answer is wanted[2..3].
    const std::vector<ChunkCoord> wanted = streamer.wanted_chunks();
    NF_CHECK(wanted.size() >= 4);
    std::unordered_set<ChunkCoord> in_flight{wanted[0], wanted[1]};
    const std::vector<ChunkCoord> next = streamer.next_wanted_loads(in_flight, 2);
    NF_CHECK_EQ(next.size(), 2u);
    NF_CHECK(next[0] == wanted[2]);
    NF_CHECK(next[1] == wanted[3]);

    // Loaded chunks are skipped the same way: load the first two for real,
    // then the answer must again start past them.
    streamer.set_max_loads_per_update(2);
    NF_CHECK_EQ(streamer.update(), 2u);
    const std::unordered_set<ChunkCoord> empty;
    const std::vector<ChunkCoord> next2 = streamer.next_wanted_loads(empty, 2);
    NF_CHECK_EQ(next2.size(), 2u);
    for (const ChunkCoord& c : next2) {
        NF_CHECK(!streamer.is_loaded(c));
    }
    NF_CHECK(next2[0] == wanted[2]);
    NF_CHECK(next2[1] == wanted[3]);
}

NF_TEST(stream_next_wanted_loads_empty_without_volume_or_cap) {
    WorldStreamer streamer;
    const std::unordered_set<ChunkCoord> empty;
    NF_CHECK(streamer.next_wanted_loads(empty, 4).empty());

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 32.0f;
    volume.load_radius = 40.0f;
    streamer.set_volume(volume);
    NF_CHECK(streamer.next_wanted_loads(empty, 0).empty());
    NF_CHECK(!streamer.next_wanted_loads(empty, 4).empty());
}

NF_TEST(stream_budget_evicts_farthest_beyond_load_radius_first) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(1000);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 32.0f;
    volume.load_radius = 40.0f;
    volume.unload_radius = 500.0f;
    streamer.set_volume(volume);
    NF_CHECK((streamer.update()) > (2u));
    const size_t loaded = streamer.loaded_chunk_count();

    // Move far away (nothing unloads by range: unload radius is huge), then
    // cap the budget below what is resident: the two nearest survivors stay.
    // Loading is disabled for this step so the count measures eviction only,
    // not eviction-plus-refill.
    volume.center = Vec3{200.0f, 0.0f, 0.0f};
    streamer.set_volume(volume);
    streamer.set_max_loaded_chunks(loaded - 2);
    streamer.set_max_loads_per_update(0);
    streamer.update();
    NF_CHECK_EQ(streamer.loaded_chunk_count(), loaded - 2);
    NF_CHECK_EQ(handlers.unloaded_log.size(), 2u);

    // Every survivor is at least as near as every evicted chunk.
    for (const ChunkCoord& kept : handlers.loaded_log) {
        if (!streamer.is_loaded(kept)) continue;
        const float kept_d =
            scene::distance_to_chunk(volume.center, kept, volume.chunk_size);
        for (const ChunkCoord& gone : handlers.unloaded_log) {
            const float gone_d =
                scene::distance_to_chunk(volume.center, gone, volume.chunk_size);
            NF_CHECK(kept_d <= gone_d + 1e-3f);
        }
    }
}

NF_TEST(stream_budget_never_evicts_must_keep_chunks) {
    RecordingHandlers handlers;
    WorldStreamer streamer;
    handlers.attach(streamer);
    streamer.set_max_loads_per_update(1000);

    scene::StreamingVolume volume;
    volume.center = Vec3{0.0f, 0.0f, 0.0f};
    volume.chunk_size = 32.0f;
    volume.load_radius = 40.0f;
    volume.unload_radius = 500.0f;
    streamer.set_volume(volume);
    NF_CHECK((streamer.update()) > (1u));
    const size_t loaded = streamer.loaded_chunk_count();

    // Everything resident is inside the load radius (must-keep). A budget of
    // 1 cannot be honoured without reloading next update, so it holds its
    // nose: over budget, but stable and evicting nothing.
    streamer.set_max_loaded_chunks(1);
    streamer.update();
    NF_CHECK_EQ(streamer.loaded_chunk_count(), loaded);
    NF_CHECK_EQ(handlers.unload_calls, 0u);
}

