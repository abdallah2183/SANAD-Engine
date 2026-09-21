#pragma once

// NF/Destruction/DebrisSink.hpp — the seam between fracture logic and whatever
// carries the shards away.
//
// Design doc Section 41 lists "debris" and "impulses" as separate concerns from
// the fracture asset itself, and demands a budget so destruction cannot kill
// the frame. Both of those are arithmetic that does not need a physics engine
// to be right, so the world never talks to Jolt directly: it talks to this
// interface, and the budget is enforced before a body is ever asked for.
//
// That split is what lets the destruction suite run on a machine with no
// physics backend at all (NullDebrisSink below) while still testing the budget,
// the throttle, and the shard kinematics exactly as the runtime sees them.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Destruction/FractureAsset.hpp>

#include <cstddef>
#include <vector>

namespace nf::destruction {

inline constexpr u32 kInvalidDebris = 0xFFFFFFFFu;

/// One shard, ready to become a rigid body. Everything a sink needs is in here,
/// so the sink never has to look at a fracture asset or a damage event, and the
/// budget never has to look at physics.
///
/// `hull_points` is the convex hull of the chunk in the *asset's* local space;
/// the world that filled this in guarantees convexity (every chunk is a slice
/// of a convex solid, see FractureMath), so a hull shape is always valid. The
/// sink rotates and offsets it by `position`/`rotation`.
struct DebrisSpawn {
    u32   chunk_id = kInvalidChunk;
    Vec3  position{0.0f, 0.0f, 0.0f};
    Quat  rotation = Quat::identity();
    Vec3  linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3  angular_velocity{0.0f, 0.0f, 0.0f};
    f32   mass = 1.0f;
    std::vector<Vec3> hull_points;
};

/// Creates and retires debris bodies. Implementations are owned by the runtime
/// layer; NFDestruction itself contains the logic, not the backend.
class IDebrisSink {
public:
    virtual ~IDebrisSink() = default;

    /// Creates the body. Returns an id the world can retire later, or
    /// kInvalidDebris when the sink could not create one (no backend, no room
    /// and nothing to evict). The world counts those refusals rather than
    /// pretending the shard exists.
    virtual u32 spawn(const DebrisSpawn& spawn) = 0;

    /// Removes a body previously returned by spawn. Called exactly once per id.
    virtual void destroy(u32 debris_id) = 0;

    /// Bodies currently alive. The budget is enforced against this.
    virtual std::size_t active_count() const = 0;
};

/// Records instead of simulating. Kept in the module rather than the tests
/// because it is also the reference implementation of the interface's
/// contract: ids are unique, destroy is idempotent for unknown ids, and
/// active_count is exactly the record count.
class NullDebrisSink final : public IDebrisSink {
public:
    struct Record {
        u32         id = kInvalidDebris;
        DebrisSpawn spawn;
        f32         age = 0.0f;
    };

    std::vector<Record> records;
    u32                 next_id = 1u;   // 0 is never handed out

    u32 spawn(const DebrisSpawn& spawn) override {
        const u32 id = next_id++;
        records.push_back(Record{id, spawn, 0.0f});
        return id;
    }

    void destroy(u32 debris_id) override {
        for (std::size_t i = 0u; i < records.size(); ++i) {
            if (records[i].id != debris_id) continue;
            records.erase(records.begin() + i);
            return;
        }
    }

    std::size_t active_count() const override { return records.size(); }

    /// The record for `debris_id`, or null when it is no longer live.
    const Record* find(u32 debris_id) const {
        for (std::size_t i = 0u; i < records.size(); ++i) {
            if (records[i].id == debris_id) return &records[i];
        }
        return nullptr;
    }
};

} // namespace nf::destruction
