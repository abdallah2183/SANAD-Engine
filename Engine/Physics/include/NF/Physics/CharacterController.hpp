#pragma once

// NF/Physics/CharacterController.hpp — dynamic-body character mover
// (design doc Section 39).
//
// A game character is a dynamic sphere whose horizontal velocity the game
// owns every tick; the solver owns gravity, contacts and pushing. That gives
// real collision (walls stop you, slopes slow you, platforms carry you
// through contacts) with none of the tunneling a teleport-mover brings, and
// it needs no sweep queries the engine does not have.
//
// Tick protocol (order matters):
//   1. controller.move(wish_dir, jump, dt)  — sets velocities
//   2. world.step(dt)                       — solves
//   3. controller.post_step()               — refreshes grounded from manifolds
//
// Grounded = a last-step contact whose normal opposes gravity within the
// slope limit. The character body never sleeps (a sleeping hero is a stuck
// hero) and never tips (rotation is solver-owned but unread).

#include <NF/Core/Math.hpp>
#include <NF/Physics/PhysicsWorld.hpp>

namespace nf::physics {

struct CharacterConfig {
    float radius = 0.4f;
    float max_speed = 6.0f; // horizontal cruise speed
    float acceleration = 40.0f; // horizontal velocity gain per second
    float air_control = 0.35f; // acceleration multiplier while airborne
    float jump_speed = 7.0f;
    float slope_limit_deg = 45.0f; // steeper contacts are walls, not ground
    float mass = 80.0f;
    float friction = 0.8f;
};

class CharacterController {
public:
    CharacterController(PhysicsWorld& world, const CharacterConfig& config, Vec3 spawn);
    ~CharacterController();

    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    /// Sets horizontal velocity toward wish_dir * max_speed (wish_dir need not
    /// be normalized; its length scales the target speed) and jumps when
    /// asked and grounded. Call before world.step().
    void move(Vec3 wish_dir, bool jump, f32 dt);

    /// Refreshes grounded() from the world's last manifolds. Call after
    /// world.step().
    void post_step();

    bool grounded() const { return m_grounded; }
    Vec3 position() const;
    Vec3 velocity() const;
    BodyHandle body() const { return m_body; }
    const CharacterConfig& config() const { return m_config; }

private:
    PhysicsWorld* m_world = nullptr;
    CharacterConfig m_config;
    BodyHandle m_body;
    bool m_grounded = false;
};

} // namespace nf::physics
