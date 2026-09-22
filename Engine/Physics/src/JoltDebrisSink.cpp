// NF/Physics/JoltDebrisSink.cpp — Jolt-backed IDebrisSink.
//
// Deliberately Jolt-header-free: this TU goes through JoltWorld's own API only.
// Every other Jolt-free TU in this module is kept that way for the same reason
// (see the note at the top of Physics/CMakeLists.txt about cross-TU Jolt
// allocation), so this one stays under /W4 /WX like its neighbours.

#include <NF/Physics/JoltDebrisSink.hpp>

#include <utility>

namespace nf::physics {

JoltDebrisSink::JoltDebrisSink(JoltWorld& world, DebrisSinkConfig config)
    : world_(world), config_(config) {}

JoltDebrisSink::~JoltDebrisSink() {
    clear();
}

u32 JoltDebrisSink::spawn(const nf::destruction::DebrisSpawn& spawn) {
    if (!world_.valid()) return nf::destruction::kInvalidDebris;

    // A hull is at least a tetrahedron, and a non-positive mass makes the
    // solver divide by it. Both are refusals rather than fallbacks: a shard
    // that quietly becomes a sphere, or a body with infinite inertia, is a
    // wrong piece of debris, which is worse than a missing one.
    if (spawn.hull_points.size() < 4u) return nf::destruction::kInvalidDebris;
    if (!(spawn.mass > 0.0f)) return nf::destruction::kInvalidDebris;

    BodyDesc desc;
    desc.type            = BodyType::Dynamic;
    desc.position        = spawn.position;
    desc.orientation     = spawn.rotation;
    desc.mass            = spawn.mass;
    desc.friction        = config_.friction;
    desc.restitution     = config_.restitution;
    desc.linear_damping  = config_.linear_damping;
    desc.angular_damping = config_.angular_damping;
    desc.allow_sleep     = config_.allow_sleep;

    // The hull is convex by construction (every chunk is a slice of a convex
    // solid, see FractureMath) and arrives in the asset's local space, so the
    // body's own transform is what places it in the world.
    const JoltBody body = world_.add_convex_hull_body(desc, spawn.hull_points);
    if (!body.valid()) return nf::destruction::kInvalidDebris;

    // BodyDesc velocities are not applied at creation, so both are set after
    // the body exists. The angular velocity is the whole point of the tear
    // torque — dropping it would send every shard flying flat.
    world_.set_linear_velocity(body, spawn.linear_velocity);
    world_.set_angular_velocity(body, spawn.angular_velocity);

    const u32 id = next_id_++;
    live_.emplace(id, body);
    chunks_.emplace(id, spawn.chunk_id);
    return id;
}

void JoltDebrisSink::destroy(u32 debris_id) {
    const auto it = live_.find(debris_id);
    if (it == live_.end()) return;
    world_.remove_body(it->second);
    live_.erase(it);
    chunks_.erase(debris_id);
}

std::size_t JoltDebrisSink::active_count() const {
    return live_.size();
}

JoltBody JoltDebrisSink::body_of(u32 debris_id) const {
    const auto it = live_.find(debris_id);
    if (it == live_.end()) return JoltBody{};
    return it->second;
}

std::vector<u32> JoltDebrisSink::live_ids() const {
    std::vector<u32> ids;
    ids.reserve(live_.size());
    for (const std::pair<const u32, JoltBody>& entry : live_) {
        ids.push_back(entry.first);
    }
    return ids;
}

u32 JoltDebrisSink::chunk_of(u32 debris_id) const {
    const auto it = chunks_.find(debris_id);
    if (it == chunks_.end()) return nf::destruction::kInvalidChunk;
    return it->second;
}

void JoltDebrisSink::clear() {
    for (const std::pair<const u32, JoltBody>& entry : live_) {
        world_.remove_body(entry.second);
    }
    live_.clear();
    chunks_.clear();
}

} // namespace nf::physics
