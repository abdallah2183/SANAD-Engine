#pragma once

// NF/Physics/VehicleSystem.hpp — ECS integration for Jolt vehicles.
//
// JoltVehicle is a physics object; the scene needs it as a COMPONENT. This
// system is the bridge: it spawns/destroys the physics vehicles for every
// VehicleComponent, applies their arcade inputs each fixed step, and writes
// the chassis pose back into scene::Transform so rendering follows physics
// (never the other way round — physics is authoritative).
//
// Ownership rules (read before touching):
//   - The JoltWorld must outlive this system. The system does NOT own the world.
//   - Each JoltVehicle is owned by the SYSTEM (a unique_ptr inside the system),
//     not by the component: a component pointing at a vehicle it does not own
//     would dangle the instant the entity is destroyed out of order.
//   - VehicleComponent::vehicle is a non-owning view, cleared on teardown.

#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp> // VehicleComponent: this system reads it
#include <NF/Physics/JoltVehicle.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Scene/Transform.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace nf::physics {

class VehicleSystem {
public:
    explicit VehicleSystem(JoltWorld& world) : m_world(&world) {}
    ~VehicleSystem() { shutdown(); }

    VehicleSystem(const VehicleSystem&) = delete;
    VehicleSystem& operator=(const VehicleSystem&) = delete;

    /// Spawns vehicles for VehicleComponent entities that have a Transform but
    /// no live vehicle yet. Call once after the scene is built and the physics
    /// world has its floor/static geometry in place.
    void startup(ecs::World& world) {
        const std::vector<ecs::Entity> entities =
            world.query<VehicleComponent, scene::Transform>();
        for (const ecs::Entity e : entities) {
            VehicleComponent* vc = world.get<VehicleComponent>(e);
            const scene::Transform* tf = world.get<scene::Transform>(e);
            if (vc == nullptr || tf == nullptr || vc->vehicle != nullptr) continue;
            const Vec3 spawn{tf->local_x, tf->local_y, tf->local_z};
            auto vehicle = std::make_unique<JoltVehicle>(*m_world, vc->config, spawn);
            if (!vehicle->valid()) continue;
            vc->vehicle = vehicle.get();
            m_vehicles.emplace(e, std::move(vehicle));
        }
    }

    /// Tears down every vehicle. Must run BEFORE the physics world dies and
    /// BEFORE the scene is destroyed, otherwise the JoltVehicle destructor
    /// touches a dead world.
    void shutdown() {
        for (auto& [entity, vehicle] : m_vehicles) {
            (void)entity;
            vehicle.reset();
        }
        m_vehicles.clear();
    }

    /// One fixed step: apply arcade inputs, then step the world OUTSIDE (the
    /// caller owns the tick — see FixedTimestep), then sync poses back.
    /// Call this exactly once per fixed step, matching world.step().
    void update(ecs::World& world) {
        for (auto& [entity, vehicle] : m_vehicles) {
            if (!world.is_alive(entity) || vehicle == nullptr) continue;
            VehicleComponent* vc = world.get<VehicleComponent>(entity);
            if (vc == nullptr) continue;
            // 1. Drive: clamp happens inside, gameplay writes raw [-1,1].
            vehicle->drive(vc->throttle, vc->steer, vc->brake, vc->handbrake);
            // 2. Write the chassis pose into the transform. The renderer reads
            //    transforms; making physics the single writer of vehicle motion
            //    is what keeps a replay and a live game visually identical.
            //    Mirrors Runtime::step_physics: physics writes local, then the
            //    transform hierarchy propagates local -> world.
            scene::Transform* tf = world.get<scene::Transform>(entity);
            if (tf == nullptr) continue;
            const JoltBodyState st = vehicle->chassis_state();
            tf->local_x = st.position.x;
            tf->local_y = st.position.y;
            tf->local_z = st.position.z;
            // Heading, not just position: a transform that only translates
            // renders a car sliding sideways through a corner. The scene
            // stores XYZ euler degrees, so the conversion lives here, in the
            // one place that knows both conventions.
            scene::euler_xyz_degrees_from_quat(st.rotation, tf->rot_x, tf->rot_y, tf->rot_z);
            tf->dirty = true;
        }
    }

    /// Per-wheel render state for every vehicle (wheel meshes, dust effects).
    /// One entry per vehicle, in query order.
    std::vector<std::vector<JoltWheelState>> wheel_states(ecs::World& world) const {
        std::vector<std::vector<JoltWheelState>> out;
        out.reserve(m_vehicles.size());
        for (const auto& [entity, vehicle] : m_vehicles) {
            if (!world.is_alive(entity) || vehicle == nullptr) continue;
            out.push_back(vehicle->wheel_states());
        }
        return out;
    }

    /// Respawn: teleports the vehicle's chassis, sets it upright and zeroes
    /// velocity. The system owns the vehicle, so the reset must go through
    /// here (not the component).
    void reset(ecs::Entity e, ecs::World& world, Vec3 position) {
        const auto it = m_vehicles.find(e);
        if (it == m_vehicles.end() || it->second == nullptr) return;
        it->second->reset(position);
        scene::Transform* tf = world.get<scene::Transform>(e);
        if (tf != nullptr) {
            tf->local_x = position.x;
            tf->local_y = position.y;
            tf->local_z = position.z;
            // vehicle_reset puts the chassis upright (identity rotation), so
            // the transform follows: a recovered car must not keep rendering
            // on its roof while the physics says it is upright.
            tf->rot_x = 0.0f;
            tf->rot_y = 0.0f;
            tf->rot_z = 0.0f;
            tf->dirty = true;
        }
    }

    usize vehicle_count() const noexcept { return m_vehicles.size(); }

private:
    JoltWorld* m_world = nullptr;
    std::unordered_map<ecs::Entity, std::unique_ptr<JoltVehicle>> m_vehicles;
};

} // namespace nf::physics
