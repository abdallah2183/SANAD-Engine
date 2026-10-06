#pragma once

// Samples/CliffStory/CliffWorld.hpp — the cliff: geometry, level contents, physics.
//
// Everything here is a *description*. NFScene2D's own rule is that components
// describe and systems own state (Components.hpp), and this follows it: the
// world holds the level and the PhysicsWorld2D that steps it, and nothing about
// how a frame is drawn leaks in.
//
// The coordinate convention is scene2d's, not a new one: x right, y DOWN. So the
// summit is at the *top* of the range and y decreases as the player climbs, and
// "height climbed" is measured against kCliffBottomY.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Scene2D/Light2D.hpp>
#include <NF/Scene2D/Physics2D.hpp>

#include <algorithm>
#include <vector>

namespace nf::cliff {

// Vec2 and Rect live in nf and nf::scene2d respectively; only the 2D types are
// pulled in here.
using scene2d::Body2D;
using scene2d::BodyHandle;
using scene2d::PhysicsWorld2D;

/// Vertical extent of the cliff. y grows downward, so kCliffTopY is the summit
/// and kCliffBottomY is the valley floor — a 130-unit climb.
constexpr f32 kCliffTopY = 2.0f;
constexpr f32 kCliffBottomY = 132.0f;
constexpr f32 kCliffHeight = kCliffBottomY - kCliffTopY;

/// Horizontal extent. Rock fills everything to the right of the face profile;
/// everything to the left is open air over the valley.
constexpr f32 kCliffLeftX = -46.0f;
constexpr f32 kCliffRightX = 44.0f;

/// World units per texture tile on the rock faces. The stone texture is 2048px,
/// so 5 units per tile gives roughly 410 px/unit of detail before the camera's
/// 40 px/unit turns it back down to about 1:10 — dense enough that the masonry
/// reads as courses of stone rather than as a repeating wallpaper.
constexpr f32 kRockUvScale = 5.0f;

/// Player tuning. Chosen against the authored ledge spacing rather than guessed:
/// a 12.0 jump under 26.0 gravity peaks at 2.77 units, and ledges are authored
/// at most 2.3 apart, so every gap has margin.
constexpr f32 kGravity = 26.0f;
constexpr f32 kRunSpeed = 8.4f;
constexpr f32 kRunAccel = 62.0f;
constexpr f32 kAirAccel = 34.0f;
constexpr f32 kJumpSpeed = 12.0f;
constexpr f32 kJumpCutFactor = 0.42f;  ///< Velocity kept when jump is released.
constexpr f32 kCoyoteTime = 0.10f;
constexpr f32 kJumpBuffer = 0.11f;
constexpr Vec2 kPlayerHalf{0.30f, 0.52f};

/// The cliff face: the x where rock meets air at height `y`.
///
/// Summed sines with mutually irrational frequencies. This is a silhouette, and
/// a silhouette wants to look irregular rather than random: the sum never
/// repeats over the cliff's height, needs no lookup table, and — being a pure
/// function of y — makes the wall collision, the ledge anchors and the drawn
/// rock agree with each other for free.
f32 cliff_face_x(f32 y);

/// A standable ledge. Most are fixed; some slide, and those need their body
/// moved by hand because a static body never moves on its own.
struct Ledge {
    f32 x = 0.0f;   ///< Top-left corner at rest.
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
    bool moving = false;
    f32 travel = 0.0f;    ///< Horizontal sweep amplitude.
    f32 speed = 0.0f;     ///< Radians per second.
    f32 phase = 0.0f;
    f32 offset = 0.0f;    ///< Current sweep offset, for drawing and physics.
    BodyHandle body;
};

/// A collectible flame — the light the climber is carrying upward.
struct Flame {
    Vec2 pos;
    f32 phase = 0.0f;
    bool taken = false;
};

/// A spike cluster. Purely a hazard volume: no physics body, because a spike
/// that blocked the player would be a wall and a spike that they stand on would
/// be a floor. Overlap is tested directly.
struct Spike {
    f32 x = 0.0f, y = 0.0f, w = 0.0f;
};

/// A patrolling bat. Purely an actor: straight-line motion plus a wing flap,
/// overlap tested directly.
struct Bat {
    Vec2 home;
    f32 range = 0.0f;
    f32 speed = 0.0f;
    f32 phase = 0.0f;
    Vec2 pos;
    f32 wing = 0.0f;
    bool alive = true;
};

/// A shrine lantern. Touching one lights it and moves the respawn point up.
struct Lantern {
    Vec2 pos;
    bool lit = false;
    f32 flame = 0.0f;  ///< Grows to 1 when lit, drives the glow and the embers.
};

/// Ivy and grass rooted on a ledge's top edge.
struct Tuft {
    Vec2 pos;
    f32 size = 0.0f;
    f32 rotation = 0.0f;
    u32 cell = 0;   ///< Which leaf of the vine sheet.
    f32 parallax = 1.0f;
};

/// A drifting speck of dust, wind-borne seed or rising ember. Hand-rolled rather
/// than Particles2D because these want to live on the *camera* rather than in
/// world space — a mote should keep drifting when the camera stops, and
/// Particles2D's emitters are anchored to a world position.
struct Mote {
    Vec2 pos;
    Vec2 vel;
    f32 life = 0.0f;
    f32 max_life = 0.0f;
    f32 size = 0.0f;
    u32 color = 0xFFFFFFFFu;
    bool glow = false;
};

/// The cliff and everything on it.
class CliffWorld {
public:
    /// Builds the level. Deterministic: no RNG state, so the same call always
    /// produces the same cliff, and a rerun is comparable frame for frame.
    void generate();

    PhysicsWorld2D& physics() { return m_phys; }
    const PhysicsWorld2D& physics() const { return m_phys; }

    /// Adds the player's body. Separated from generate() because the world is
    /// the static level and the player is the one body that gets recreated on
    /// every respawn.
    BodyHandle create_player(Vec2 position);

    /// Moves the sliding ledges to time `t`. Called once per fixed step, never
    /// per rendered frame, or a fast machine would slide them further per second.
    void step_movers(f32 t);

    /// True when `handle` rests on something. Reads the contact list rather
    /// than raycasting: a resolved contact pointing downward IS "feet on
    /// ground", which is the engine's own framing (Physics2D.hpp).
    bool grounded(BodyHandle handle) const;

    f32 face_x(f32 y) const { return cliff_face_x(y); }

    const std::vector<Ledge>& ledges() const { return m_ledges; }
    const std::vector<Flame>& flames() const { return m_flames; }
    std::vector<Flame>& flames() { return m_flames; }
    const std::vector<Spike>& spikes() const { return m_spikes; }
    std::vector<Bat>& bats() { return m_bats; }
    const std::vector<Lantern>& lanterns() const { return m_lanterns; }
    std::vector<Lantern>& lanterns() { return m_lanterns; }
    const std::vector<Tuft>& tufts() const { return m_tufts; }

    /// Where the shrine platform sits, and the summit trigger height.
    f32 summit_y() const { return kCliffTopY; }
    Vec2 shrine_pos() const { return m_shrine; }
    Vec2 spawn_pos() const { return m_spawn; }

    /// Progress in [0, 1] from the valley floor to the summit.
    f32 progress(f32 player_y) const {
        return std::clamp((kCliffBottomY - player_y) / kCliffHeight, 0.0f, 1.0f);
    }

    /// Motes are owned by the world because they are ambience, not gameplay,
    /// and they keep living while the camera rests.
    std::vector<Mote>& motes() { return m_motes; }
    void step_motes(f32 dt, const Vec2& camera_pos, f32 view_w, f32 view_h);

private:
    void build_rock_bodies();
    void build_ledges();
    void build_contents();

    /// Deterministic hash in [0,1). Replaces an RNG so the level is a pure
    /// function of its seed and nothing carries state between calls.
    static f32 hash01(u32 seed);

    PhysicsWorld2D m_phys;

    std::vector<Ledge> m_ledges;
    std::vector<Flame> m_flames;
    std::vector<Spike> m_spikes;
    std::vector<Bat> m_bats;
    std::vector<Lantern> m_lanterns;
    std::vector<Tuft> m_tufts;
    std::vector<Mote> m_motes;

    Vec2 m_spawn{-6.0f, kCliffBottomY - 2.0f};
    Vec2 m_shrine{2.0f, kCliffTopY + 2.0f};
};

} // namespace nf::cliff