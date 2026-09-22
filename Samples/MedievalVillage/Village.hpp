#pragma once

// Samples/MedievalVillage/Village.hpp — the village layout.
//
// The kit is modular: walls are 2.00 m wide and 3.12 m tall, floors are 2x2 m,
// and the roofs come in a handful of measured sizes (a "4x4" roof is 5.51 m
// across, a "6x6" is 8.25 m). Every placement below is therefore expressed in
// terms of what the importer measured, never in terms of what a piece's name
// suggests it should be.
//
// The layout is generated, not hand-listed: `add_house` builds a square of N
// wall modules per side with a door on one side and a window on another, and the
// four houses differ only in their kit choices. That keeps the piece count low
// enough to read while still exercising walls, corners, floors, roofs, doors,
// frames, windows, shutters, chimneys and vines.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::rendering {
class StaticMesh;
}

namespace nf::sample::medieval {

class Kit;

/// What a placed piece is for. The game creates entities from this list and
/// switches on the tag to decide colliders and interaction.
enum class PlacementTag {
    Structure, // walls, floors, roofs, corners, frames — static box collider
    Decor,     // vines, shutters, borders — drawn, no collider
    Crate,     // a pickup the player can carry
    Wagon,     // the delivery point
    Door,      // swings open on interaction
    Fence,     // static box collider, shorter than a wall
};

struct PlacedPiece {
    std::string piece;      // kit piece name
    Vec3 position{};        // world position of the piece's local origin
    float rot_y = 0.0f;     // degrees
    Vec3 scale{1.0f, 1.0f, 1.0f};
    PlacementTag tag = PlacementTag::Structure;
};

struct Village {
    std::vector<PlacedPiece> pieces;
    Vec3 spawn{0.0f, 1.0f, 10.0f};
    Vec3 wagon{0.0f, 0.0f, -2.0f};
    /// Half extent of the flat playable ground, in metres.
    float ground_half_extent = 34.0f;
};

/// Builds the village from the loaded kit. Pieces the kit does not contain are
/// skipped, so a partial kit still produces a placeable (if sparser) village.
Village build_village(const Kit& kit);

/// A single flat grid mesh for the ground, with UV0 tiled so a 2 m floor
/// texture does not stretch across the whole map. One mesh, one draw call —
/// tiling the ground with kit floor pieces would cost ~290 entities.
std::unique_ptr<rendering::StaticMesh> make_ground_grid(float half_extent, float tile_size);

/// World-space AABB of a placed piece (its local bounds through the placement's
/// rotation and scale). Rotation is general: the eight corners are transformed
/// and re-bounded, so a door caught mid-swing still has a correct box.
struct WorldAabb {
    Vec3 min{};
    Vec3 max{};
    Vec3 centre() const { return (min + max) * 0.5f; }
    Vec3 size() const { return max - min; }
};
WorldAabb placed_world_aabb(const Kit& kit, const PlacedPiece& placed);

} // namespace nf::sample::medieval
