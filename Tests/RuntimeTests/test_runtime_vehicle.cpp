// RuntimeTests — VehicleSystem: the physics -> scene bridge for Jolt vehicles.
//
// VehicleSystem.hpp is header-only, so until a translation unit includes it
// nothing in the build proves it even COMPILES (Rule 0: code the build never
// translates is code nobody verified). These cases translate it and pin what
// it writes back into scene::Transform: the chassis position AND the chassis
// heading. A vehicle whose transform only translates renders as a box sliding
// sideways through every corner, and a net snapshot has no heading to carry.

#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/VehicleSystem.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

namespace {

BodyDesc static_plane() {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_plane(Vec3{0, 1, 0});
    return d;
}

/// One car dropped on a plane, driven for `steps` fixed ticks at `steer`.
class Rig {
public:
    Rig() {
        m_world.add_body(static_plane());
        m_car = m_ecs.create_entity();
        m_ecs.add<scene::Transform>(m_car, scene::Transform{});
        m_ecs.add<VehicleComponent>(m_car, VehicleComponent{});
        m_ecs.get<scene::Transform>(m_car)->local_y = 2.0f;
        m_system.startup(m_ecs);
    }

    void run(int steps, float steer) {
        VehicleComponent* vc = m_ecs.get<VehicleComponent>(m_car);
        vc->throttle = 1.0f;
        vc->steer = steer;
        for (int i = 0; i < steps; ++i) {
            m_system.update(m_ecs);
            m_world.step(1.0f / 60.0f);
        }
        m_system.update(m_ecs); // the pose write precedes the step, so sync once more
    }

    /// The orientation the scene ended up with, as a quaternion: comparing
    /// rotations rather than euler triple keeps the assertions independent of
    /// how Transform spells an angle (wrapping, gimbal-sign, etc).
    Quat heading() const {
        const scene::Transform* tf = m_ecs.get<scene::Transform>(m_car);
        return scene::quat_from_euler_xyz_degrees(tf->rot_x, tf->rot_y, tf->rot_z);
    }

    const scene::Transform* transform() const { return m_ecs.get<scene::Transform>(m_car); }
    VehicleSystem& system() { return m_system; }
    ecs::Entity car() const { return m_car; }
    ecs::World& world() { return m_ecs; }

private:
    JoltWorld m_world;
    ecs::World m_ecs;
    VehicleSystem m_system{m_world};
    ecs::Entity m_car = ecs::kInvalidEntity;
};

} // namespace

NF_TEST(vehicle_system_spawns_one_vehicle_per_component) {
    Rig rig;
    NF_CHECK(rig.system().vehicle_count() == 1);
    // The component holds a non-owning view; the system owns the vehicle.
    NF_CHECK(rig.world().get<VehicleComponent>(rig.car())->vehicle != nullptr);
}

NF_TEST(vehicle_system_writes_chassis_position_to_transform) {
    Rig rig;
    rig.run(180, 0.0f);
    const scene::Transform* tf = rig.transform();
    NF_CHECK(tf->local_z > 0.5f); // forward is +z, so it really drove off the spawn
    NF_CHECK(tf->local_y > 0.1f && tf->local_y < 2.0f); // settled, not launched
    NF_CHECK(tf->dirty); // renderer must re-upload
}

NF_TEST(vehicle_system_writes_chassis_heading_to_transform) {
    // Same throttle, different steering: the TRANSFORM must end up pointing
    // somewhere else. Before this change the system wrote position only, and
    // these two rigs differed in translation but not in orientation at all.
    Rig straight;
    straight.run(240, 0.0f);
    Rig turned;
    turned.run(240, 0.8f);

    const Quat qs = straight.heading();
    const Quat qt = turned.heading();
    NF_CHECK_NEAR(qs.length_sq(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(qt.length_sq(), 1.0f, 1e-4f);
    // |dot| == 1 means "identical orientation"; a real turn has to read below.
    NF_CHECK(std::fabs(qs.dot(qt)) < 0.999f);
}

NF_TEST(vehicle_system_reset_levels_the_transform) {
    Rig rig;
    rig.run(240, 0.8f);
    // The car really is rotated before the reset, otherwise the assertions
    // below would pass on a car that was never turned in the first place.
    NF_CHECK(std::fabs(rig.heading().w) < 0.999f);

    rig.system().reset(rig.car(), rig.world(), Vec3{0, 2, 0});
    const scene::Transform* tf = rig.transform();
    // vehicle_reset stands the chassis upright, so the euler angles go with it:
    // a recovered car must not keep rendering on its roof.
    NF_CHECK_NEAR(tf->rot_x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(tf->rot_y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(tf->rot_z, 0.0f, 1e-5f);
    NF_CHECK_NEAR(tf->local_x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(tf->local_y, 2.0f, 1e-5f);
    NF_CHECK_NEAR(tf->local_z, 0.0f, 1e-5f);
}
