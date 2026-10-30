#pragma once

// NF/Physics/TriggerVolume.hpp — Spatial Trigger Volumes & Overlap Event Manager

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/Shapes.hpp>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace nf::physics {

enum class TriggerEventType : u8 {
    Enter = 0,
    Stay,
    Exit
};

struct TriggerEvent {
    u32 trigger_id = 0;
    BodyHandle body{};
    TriggerEventType type = TriggerEventType::Enter;
};

using TriggerCallback = std::function<void(const TriggerEvent&)>;

/// A non-solid spatial volume that detects overlapping bodies without physical reaction forces.
struct TriggerVolume {
    u32 id = 0;
    std::string name;
    Shape shape = Shape::make_box(Vec3(1.0f, 1.0f, 1.0f));
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat orientation = Quat::identity();
    u32 layer_mask = 0xFFFFFFFF;
    bool enabled = true;
};

/// Manages multiple TriggerVolumes, evaluates overlap states each frame/fixed-step,
/// and dispatches OnTriggerEnter, OnTriggerStay, and OnTriggerExit.
class TriggerManager {
public:
    TriggerManager() = default;

    u32 add_volume(const TriggerVolume& volume);
    bool remove_volume(u32 volume_id);
    void clear();

    TriggerVolume* get_volume(u32 volume_id);
    const TriggerVolume* get_volume(u32 volume_id) const;
    size_t volume_count() const noexcept { return m_volumes.size(); }

    void register_callback(TriggerCallback callback);

    /// Evaluates current physics world bodies against active trigger volumes.
    /// Emits Enter, Stay, and Exit events accordingly.
    void update(const PhysicsWorld& world);

    /// Returns true if a body is currently inside a given trigger.
    bool is_overlapping(u32 volume_id, BodyHandle body) const;

    /// Gets all bodies currently inside a given trigger volume.
    std::vector<BodyHandle> get_overlapping_bodies(u32 volume_id) const;

private:
    u32 m_next_id = 1;
    std::vector<TriggerVolume> m_volumes;
    std::vector<TriggerCallback> m_callbacks;

    // Map: volume_id -> set of active BodyHandles encoded as uint64_t
    std::unordered_map<u32, std::unordered_set<uint64_t>> m_active_overlaps;

    static uint64_t encode_handle(BodyHandle h) noexcept {
        return (static_cast<uint64_t>(h.generation) << 32) | static_cast<uint64_t>(h.index);
    }

    static BodyHandle decode_handle(uint64_t key) noexcept {
        return BodyHandle{
            static_cast<u32>(key & 0xFFFFFFFF),
            static_cast<u32>(key >> 32)
        };
    }

    void emit_event(const TriggerEvent& event);
};

} // namespace nf::physics
