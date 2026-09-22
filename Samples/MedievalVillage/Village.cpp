// Samples/MedievalVillage/Village.cpp — see Village.hpp.

#include "Village.hpp"

#include "KitAssets.hpp"

#include <NF/Core/Logger.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <algorithm>
#include <cmath>

namespace nf::sample::medieval {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Rotates a vector about the world Y axis, using the SAME convention as
/// scene::compose_trs_mat4 (R = Ry * Rx * Rz, row-vector application):
///   x' = x*cos + z*sin ,  z' = -x*sin + z*cos
/// The layout maths below and the matrix the renderer builds must agree, or
/// every wall would be placed for one rotation and drawn with another.
Vec3 rotate_y(const Vec3& v, float deg) {
    const float r = deg * kPi / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

/// Places `piece` so that its local bounding-box centre in X/Z lands on
/// (x, z) and its local bounding-box MINIMUM y lands on `ground_y`.
///
/// Anchoring on measured bounds rather than on the origin is what makes the
/// layout independent of how the kit's exporter happened to place each piece's
/// pivot: the plaster wall's box spans z in [-0.314, 0.092], so its origin is
/// 11 cm off its own centre, and a wall placed by origin would sit 11 cm out of
/// line with the wall beside it.
bool place_grounded(const Kit& kit, std::vector<PlacedPiece>& out, const char* piece_name,
                    float x, float ground_y, float z, float rot_y, PlacementTag tag,
                    const Vec3& scale = Vec3{1.0f, 1.0f, 1.0f}) {
    const KitPiece* p = kit.piece(piece_name);
    if (p == nullptr) {
        return false;
    }
    const Vec3 lc = p->centre();
    const Vec3 rc = rotate_y(Vec3{lc.x, 0.0f, lc.z}, rot_y);
    PlacedPiece placed;
    placed.piece = piece_name;
    placed.position = Vec3{x - rc.x, ground_y - p->bounds_min.y, z - rc.z};
    placed.rot_y = rot_y;
    placed.scale = scale;
    placed.tag = tag;
    out.push_back(std::move(placed));
    return true;
}

struct HouseSpec {
    float cx = 0.0f;
    float cz = 0.0f;
    float rot_y = 0.0f;      // whole-house yaw, degrees
    int modules = 3;         // wall pieces per side; the side is 2*modules metres

    const char* wall = "Wall_Plaster_Straight";
    const char* wall_door = "Wall_Plaster_Door_Flat";
    const char* wall_window = "Wall_Plaster_Window_Wide_Flat";
    const char* floor = "Floor_WoodDark";
    const char* roof = "Roof_RoundTiles_6x6";
    const char* corner = "Corner_Exterior_Brick";
    const char* door = "Door_1_Flat";
    const char* frame = "DoorFrame_Flat_WoodDark";
    const char* window = "Window_Wide_Flat1";
    const char* shutters = "WindowShutters_Wide_Flat_Open";
    bool chimney = true;

    int door_side = 0;   // 0 = +Z, 1 = +X, 2 = -Z, 3 = -X (house-local)
    int window_side = 1;
};

/// Outward normal of a house side, in house-local space, and the yaw a piece on
/// that side needs so its exterior face points outward. The yaws are the four
/// quarter turns, and they follow from rotate_y's convention: at 90 degrees the
/// local +z axis (a window's outward face) maps onto world +x, which is the +X
/// side's outward direction.
struct SideBasis {
    float nx = 0.0f;
    float nz = 0.0f;
    float rot_y = 0.0f;
};

SideBasis side_basis(int side) {
    switch (side & 3) {
    case 0: return {0.0f, 1.0f, 0.0f};    // +Z
    case 1: return {1.0f, 0.0f, 90.0f};   // +X
    case 2: return {0.0f, -1.0f, 180.0f}; // -Z
    default: return {-1.0f, 0.0f, 270.0f};// -X
    }
}

/// Builds one house: walls (with a door opening and a window opening), corner
/// columns, a tiled floor, a roof, and the door/window/servant details.
void add_house(const Kit& kit, std::vector<PlacedPiece>& out, const HouseSpec& spec) {
    const float h = static_cast<float>(spec.modules); // half extent, metres
    const KitPiece* wall = kit.piece(spec.wall);
    if (wall == nullptr) {
        return;
    }
    const float wall_top = wall->bounds_max.y;

    // House-local (lx, lz) -> world. The house yaw composes with each piece's
    // own side yaw, which is why the placement goes through rotate_y twice.
    const auto to_world = [&](float lx, float lz) -> Vec3 {
        const Vec3 r = rotate_y(Vec3{lx, 0.0f, lz}, spec.rot_y);
        return Vec3{spec.cx + r.x, 0.0f, spec.cz + r.z};
    };

    // --- walls -------------------------------------------------------------
    const int mid = spec.modules / 2;
    for (int side = 0; side < 4; ++side) {
        const SideBasis sb = side_basis(side);
        // Tangent along the side: the normal turned a quarter turn.
        const float tx = -sb.nz;
        const float tz = sb.nx;
        for (int i = 0; i < spec.modules; ++i) {
            const float t = -h + 1.0f + 2.0f * static_cast<float>(i);
            const float lx = sb.nx * h + tx * t;
            const float lz = sb.nz * h + tz * t;
            const Vec3 w = to_world(lx, lz);
            const float yaw = spec.rot_y + sb.rot_y;

            const char* which = spec.wall;
            if (i == mid && side == spec.door_side) {
                which = spec.wall_door;
            } else if (i == mid && side == spec.window_side) {
                which = spec.wall_window;
            }
            place_grounded(kit, out, which, w.x, 0.0f, w.z, yaw, PlacementTag::Structure);
        }

        // Corner column at this side's far end (each corner is claimed once).
        const Vec3 corner_w = to_world(sb.nx * h, sb.nz * h);
        place_grounded(kit, out, spec.corner, corner_w.x, 0.0f, corner_w.z,
                       spec.rot_y + sb.rot_y, PlacementTag::Structure);
    }

    // --- floor -------------------------------------------------------------
    // Base sunk by the tile's own thickness so the floor SURFACE is exactly the
    // physics ground plane at y = 0. A floor laid with its base at y = 0 would
    // leave the player's feet 2 cm below the boards they are standing on.
    const KitPiece* floor_piece = kit.piece(spec.floor);
    const float floor_sink = (floor_piece != nullptr)
                                 ? (floor_piece->bounds_max.y - floor_piece->bounds_min.y)
                                 : 0.02f;
    for (int i = 0; i < spec.modules; ++i) {
        for (int j = 0; j < spec.modules; ++j) {
            const float lx = -h + 1.0f + 2.0f * static_cast<float>(i);
            const float lz = -h + 1.0f + 2.0f * static_cast<float>(j);
            const Vec3 w = to_world(lx, lz);
            place_grounded(kit, out, spec.floor, w.x, -floor_sink, w.z, spec.rot_y,
                           PlacementTag::Decor);
        }
    }

    // --- roof --------------------------------------------------------------
    // The roof's local minimum y is its eave, so grounding it at the wall top
    // is what makes it sit ON the walls rather than through them.
    {
        const Vec3 w = to_world(0.0f, 0.0f);
        place_grounded(kit, out, spec.roof, w.x, wall_top, w.z, spec.rot_y,
                       PlacementTag::Structure);
    }

    // --- door --------------------------------------------------------------
    {
        const SideBasis sb = side_basis(spec.door_side);
        const Vec3 w = to_world(sb.nx * h, sb.nz * h);
        const float yaw = spec.rot_y + sb.rot_y;
        place_grounded(kit, out, spec.frame, w.x, 0.0f, w.z, yaw, PlacementTag::Structure);
        place_grounded(kit, out, spec.door, w.x, 0.0f, w.z, yaw, PlacementTag::Door);
    }

    // --- window ------------------------------------------------------------
    {
        const SideBasis sb = side_basis(spec.window_side);
        const Vec3 w = to_world(sb.nx * h, sb.nz * h);
        const float yaw = spec.rot_y + sb.rot_y;
        place_grounded(kit, out, spec.window, w.x, 0.0f, w.z, yaw, PlacementTag::Decor);
        // Shutters flank the opening, one either side along the wall.
        const float tx = -sb.nz;
        const float tz = sb.nx;
        for (float off : {-0.95f, 0.95f}) {
            const Vec3 sw = to_world(sb.nx * h + tx * off, sb.nz * h + tz * off);
            place_grounded(kit, out, spec.shutters, sw.x, 0.0f, sw.z, yaw, PlacementTag::Decor);
        }
    }

    // --- chimney -----------------------------------------------------------
    if (spec.chimney) {
        const Vec3 w = to_world(h * 0.45f, h * -0.35f);
        place_grounded(kit, out, "Prop_Chimney", w.x, wall_top + 2.4f, w.z, spec.rot_y,
                       PlacementTag::Decor);
    }

    // --- garden fence ------------------------------------------------------
    // A short run of fence beside the house, on the side that is not the door.
    {
        const int fence_side = (spec.door_side + 2) & 3;
        const SideBasis sb = side_basis(fence_side);
        const float tx = -sb.nz;
        const float tz = sb.nx;
        const char* fence[] = {"Prop_WoodenFence_Single", "Prop_WoodenFence_Extension1",
                               "Prop_WoodenFence_Extension1"};
        for (int i = 0; i < 3; ++i) {
            const float t = (static_cast<float>(i) - 1.0f) * 2.0f + 4.0f;
            const float lx = sb.nx * (h + 2.0f) + tx * t;
            const float lz = sb.nz * (h + 2.0f) + tz * t;
            const Vec3 w = to_world(lx, lz);
            place_grounded(kit, out, fence[i], w.x, 0.0f, w.z, spec.rot_y + sb.rot_y,
                           PlacementTag::Fence);
        }
    }
}

} // namespace

Village build_village(const Kit& kit) {
    Village v;

    // --- the four houses ---------------------------------------------------
    // Plaster houses front the plaza; the older, brick ones sit behind them, so
    // the village reads as a place with a history rather than one building
    // copied four times.
    HouseSpec miller;
    miller.cx = -9.0f; miller.cz = 9.0f;
    miller.wall = "Wall_Plaster_Straight";
    miller.wall_door = "Wall_Plaster_Door_Flat";
    miller.wall_window = "Wall_Plaster_Window_Wide_Flat";
    miller.floor = "Floor_WoodDark";
    miller.roof = "Roof_RoundTiles_6x6";
    miller.corner = "Corner_Exterior_Brick";
    miller.door = "Door_1_Flat";
    miller.door_side = 1;    // door faces the plaza (+X)
    miller.window_side = 0;
    add_house(kit, v.pieces, miller);

    HouseSpec inn = miller;
    inn.cx = 9.0f; inn.cz = 9.0f;
    inn.wall = "Wall_Plaster_WoodGrid";
    inn.floor = "Floor_WoodLight";
    inn.roof = "Roof_RoundTiles_6x6";
    inn.door = "Door_2_Flat";
    inn.door_side = 3;       // door faces the plaza (-X)
    inn.window_side = 2;
    add_house(kit, v.pieces, inn);

    HouseSpec smithy;
    smithy.cx = -9.0f; smithy.cz = -9.0f;
    smithy.wall = "Wall_UnevenBrick_Straight";
    smithy.wall_door = "Wall_UnevenBrick_Door_Flat";
    smithy.wall_window = "Wall_UnevenBrick_Window_Wide_Flat";
    smithy.floor = "Floor_UnevenBrick";
    smithy.roof = "Roof_RoundTiles_6x6";
    smithy.corner = "Corner_ExteriorWide_Brick";
    smithy.door = "Door_4_Flat";
    smithy.door_side = 1;
    smithy.window_side = 2;
    add_house(kit, v.pieces, smithy);

    HouseSpec store;
    store.cx = 9.0f; store.cz = -9.0f;
    store.wall = "Wall_UnevenBrick_Straight";
    store.wall_door = "Wall_UnevenBrick_Door_Flat";
    store.wall_window = "Wall_UnevenBrick_Window_Wide_Flat";
    store.floor = "Floor_Brick";
    store.roof = "Roof_RoundTiles_6x6";
    store.corner = "Corner_Exterior_Brick";
    store.door = "Door_1_Round";
    store.door_side = 3;
    store.window_side = 0;
    add_house(kit, v.pieces, store);

    // --- plaza kerb --------------------------------------------------------
    // A stone border around the square the houses face, so the play space has a
    // readable edge without a wall around it.
    {
        constexpr float k = 6.0f;
        for (int side = 0; side < 4; ++side) {
            const SideBasis sb = side_basis(side);
            const float tx = -sb.nz;
            const float tz = sb.nx;
            for (int i = -3; i <= 3; ++i) {
                const float t = static_cast<float>(i) * 2.0f;
                const float x = sb.nx * k + tx * t;
                const float z = sb.nz * k + tz * t;
                place_grounded(kit, v.pieces, "Prop_ExteriorBorder_Straight1", x, 0.0f, z,
                               sb.rot_y, PlacementTag::Decor);
            }
        }
    }

    // --- the delivery wagon ------------------------------------------------
    v.wagon = Vec3{0.0f, 0.0f, -2.0f};
    place_grounded(kit, v.pieces, "Prop_Wagon", v.wagon.x, 0.0f, v.wagon.z, 90.0f,
                   PlacementTag::Wagon);

    // --- supply crates -----------------------------------------------------
    // Spread around and behind the houses so collecting them means walking the
    // village rather than crossing the square six times.
    const Vec3 crates[] = {
        {-12.5f, 0.0f, 3.0f},  {12.0f, 0.0f, -4.5f}, {3.5f, 0.0f, -14.0f},
        {-4.5f, 0.0f, 14.5f},  {14.5f, 0.0f, 12.0f}, {-15.0f, 0.0f, -12.5f},
    };
    for (const Vec3& c : crates) {
        place_grounded(kit, v.pieces, "Prop_Crate", c.x, 0.0f, c.z, 0.0f, PlacementTag::Crate);
    }

    // --- vines on the plaza-facing walls -----------------------------------
    // The vine's local box runs BELOW its origin (y in [-2.12, 0.48]), so it is
    // anchored by its top: grounding it at 2.4 m hangs it down the wall face.
    {
        const Vec3 vines[] = {
            {-5.85f, 2.4f, 9.0f},  {5.85f, 2.4f, 9.0f},
            {-5.85f, 2.4f, -9.0f}, {5.85f, 2.4f, -9.0f},
        };
        const float yaws[] = {270.0f, 90.0f, 270.0f, 90.0f};
        for (int i = 0; i < 4; ++i) {
            const KitPiece* vine = kit.piece("Prop_Vine1");
            if (vine == nullptr) {
                break;
            }
            PlacedPiece p;
            p.piece = "Prop_Vine1";
            // Anchored by its top edge, not its base: this piece hangs.
            const Vec3 lc = vine->centre();
            p.position = Vec3{vines[i].x - lc.x, vines[i].y - vine->bounds_max.y, vines[i].z - lc.z};
            p.rot_y = yaws[i];
            p.tag = PlacementTag::Decor;
            v.pieces.push_back(std::move(p));
        }
    }

    NF_LOG_INFO(LogCategory::Core, "MedievalVillage: layout has {} placed pieces", v.pieces.size());
    return v;
}

std::unique_ptr<rendering::StaticMesh> make_ground_grid(float half_extent, float tile_size) {
    const float size = half_extent * 2.0f;
    int divisions = static_cast<int>(std::ceil(size / tile_size));
    if (divisions < 1) {
        divisions = 1;
    }
    const float step = size / static_cast<float>(divisions);

    auto mesh = std::make_unique<rendering::StaticMesh>("GroundGrid");
    auto& lod = mesh->lod(0);
    const int side = divisions + 1;
    lod.vertices.resize(static_cast<usize>(side) * static_cast<usize>(side));
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const float x = -half_extent + step * static_cast<float>(i);
            const float z = -half_extent + step * static_cast<float>(j);
            rendering::Vertex& vert = lod.vertices[static_cast<usize>(j * side + i)];
            vert.position[0] = x;
            vert.position[1] = 0.0f;
            vert.position[2] = z;
            vert.normal[0] = 0.0f;
            vert.normal[1] = 1.0f;
            vert.normal[2] = 0.0f;
            vert.tangent[0] = 1.0f;
            vert.tangent[3] = 1.0f;
            // UV0 tiles the texture once per `tile_size` metres, so a 2 m floor
            // map repeats across the ground instead of stretching over it.
            vert.uv0[0] = x / tile_size;
            vert.uv0[1] = z / tile_size;
        }
    }
    lod.indices.reserve(static_cast<usize>(divisions) * static_cast<usize>(divisions) * 6);
    for (int j = 0; j < divisions; ++j) {
        for (int i = 0; i < divisions; ++i) {
            const u32 i0 = static_cast<u32>(j * side + i);
            const u32 i1 = i0 + 1;
            const u32 i2 = static_cast<u32>((j + 1) * side + i);
            const u32 i3 = i2 + 1;
            // Wound so the surface faces +Y with the renderer's front-face
            // convention (the quad helper documents the same trap).
            lod.indices.push_back(i2);
            lod.indices.push_back(i0);
            lod.indices.push_back(i1);
            lod.indices.push_back(i1);
            lod.indices.push_back(i3);
            lod.indices.push_back(i2);
        }
    }
    rendering::SubMesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0;
    lod.submeshes.push_back(sm);
    lod.material_slots.push_back(rendering::MaterialSlot{"ground"});
    rendering::StaticMesh::compute_lod_bounds(lod);
    return mesh;
}

WorldAabb placed_world_aabb(const Kit& kit, const PlacedPiece& placed) {
    WorldAabb out;
    const KitPiece* p = kit.piece(placed.piece);
    if (p == nullptr) {
        return out;
    }
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 local{(corner & 1) ? p->bounds_max.x : p->bounds_min.x,
                         (corner & 2) ? p->bounds_max.y : p->bounds_min.y,
                         (corner & 4) ? p->bounds_max.z : p->bounds_min.z};
        const Vec3 scaled{local.x * placed.scale.x, local.y * placed.scale.y,
                          local.z * placed.scale.z};
        const Vec3 rotated = rotate_y(scaled, placed.rot_y);
        const Vec3 world = rotated + placed.position;
        if (first) {
            out.min = world;
            out.max = world;
            first = false;
            continue;
        }
        out.min.x = std::min(out.min.x, world.x);
        out.min.y = std::min(out.min.y, world.y);
        out.min.z = std::min(out.min.z, world.z);
        out.max.x = std::max(out.max.x, world.x);
        out.max.y = std::max(out.max.y, world.y);
        out.max.z = std::max(out.max.z, world.z);
    }
    return out;
}

} // namespace nf::sample::medieval
