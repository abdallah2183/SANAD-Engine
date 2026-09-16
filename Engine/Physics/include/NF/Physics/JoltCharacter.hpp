#pragma once

// NF/Physics/JoltCharacter.hpp — playable character on Jolt (Phase 16, §39).
//
// A capsule character (Jolt CharacterVirtual) driven by gameplay inputs:
// walk/accelerate toward a wish direction, gravity + jump, walk up obstacles
// no taller than the configured step offset, stop at slopes steeper than
// max_slope_deg, ride moving platforms, crouch, and climb (a hook the game
// flips on while the character is on a ladder). Deterministic for identical
// inputs and dt, so a network layer can replay it for client prediction.
//
// Kinematic-style: this is NOT a rigid body. It does not appear in
// body_count(), the world step does not move it (move() does), and it pushes
// dynamics only through Jolt's "standing mass" force. The world must outlive
// the character. No rendering coupling: read position()/velocity() and pose
// your mesh.
//
// IMPLEMENTATION NOTE: every Jolt object lives in JoltWorld.cpp (single TU
// owning the PhysicsSystem). This header stays Jolt-free; the class is a thin
// handle over JoltWorld::CharacterHandle. Do NOT move Jolt calls into
// JoltCharacter.cpp — cross-TU Jolt allocation heap-corrupts (custom Jolt
// allocator + per-TU operator new/delete).

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Physics/JoltWorld.hpp>

namespace nf::physics {

class JoltCharacter {
public:
    JoltCharacter(JoltWorld& world, const JoltCharacterConfig& config, Vec3 spawn);
    ~JoltCharacter();

    JoltCharacter(const JoltCharacter&) = delete;
    JoltCharacter& operator=(const JoltCharacter&) = delete;

    bool valid() const;

    /// One gameplay tick (see JoltWorld::character_move). Call once per fixed
    /// step; the world step is still the game's to call for everything else.
    void move(Vec3 wish_dir, bool jump, float dt);
    /// Ladder hook: while enabled, wish_dir.y drives vertical movement.
    void set_climbing(bool enabled);
    void set_crouch(bool crouched);

    bool is_crouched() const;
    bool is_grounded() const;
    /// Feet (the bottom of the capsule), not the centre.
    Vec3 position() const;
    Vec3 velocity() const;

private:
    JoltWorld* m_world = nullptr;
    JoltWorld::CharacterHandle m_handle;
    bool m_ok = false;
};

} // namespace nf::physics
