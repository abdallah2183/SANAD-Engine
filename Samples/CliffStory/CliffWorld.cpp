// Samples/CliffStory/CliffWorld.cpp — the cliff: profile, level layout, physics bodies.

#include "CliffWorld.hpp"

#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <cmath>

namespace nf::cliff {

namespace {

/// Incommensurate frequencies. Nothing here shares a period, so the sum never
/// visibly loops over the cliff's 130-unit height.
constexpr f32 kFaceAmp1 = 2.60f;
constexpr f32 kFaceFreq1 = 0.137f;
constexpr f32 kFaceAmp2 = 1.15f;
constexpr f32 kFaceFreq2 = 0.311f;
constexpr f32 kFaceAmp3 = 0.42f;
constexpr f32 kFaceFreq3 = 0.733f;
/// Rock starts here and runs right, so the player always has a wall at their
/// back and open air in front.
constexpr f32 kFaceOffset = 3.0f;

/// Ledges are authored bottom-up with this vertical budget. A 12.0 jump under
/// 26.0 gravity peaks at 2.77 units, so 1.5-2.3 keeps every gap clearable with
/// room to spare while still asking something of the player.
constexpr f32 kLedgeGapMin = 1.5f;
constexpr f32 kLedgeGapMax = 2.3f;
constexpr f32 kLedgeWidthMin = 2.4f;
constexpr f32 kLedgeWidthMax = 4.6f;
/// How far a ledge may sit to the left of the one below it. Horizontal reach is
/// ~7 units at full run, so this is a comfortable ask rather than a pixel hunt.
constexpr f32 kLedgeReachMax = 4.0f;

constexpr u32 kSeed = 0x5EEDC11Fu;

} // namespace

f32 cliff_face_x(f32 y) {
    return kFaceOffset + kFaceAmp1 * std::sin(y * kFaceFreq1) +
           kFaceAmp2 * std::sin(y * kFaceFreq2 + 1.7f) +
           kFaceAmp3 * std::sin(y * kFaceFreq3 + 0.4f);
}

f32 CliffWorld::hash01(u32 seed) {
    // Integer avalanche, then take the top 24 bits. Deterministic and
    // independent of platform floating point, unlike a std::mt19937 threaded
    // through global state.
    u32 h = seed * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return static_cast<f32>(h >> 8) / static_cast<f32>(1u << 24);
}

void CliffWorld::generate() {
    m_phys.clear();
    m_ledges.clear();
    m_flames.clear();
    m_spikes.clear();
    m_bats.clear();
    m_lanterns.clear();
    m_tufts.clear();
    m_motes.clear();

    m_phys.set_gravity(Vec2{0.0f, kGravity});

    build_rock_bodies();
    build_ledges();
    build_contents();

    NF_LOG_INFO(LogCategory::Core,
                "CliffStory: cliff built — {} ledges, {} flames, {} spikes, {} bats, "
                "{} lanterns, {} tufts",
                m_ledges.size(), m_flames.size(), m_spikes.size(), m_bats.size(),
                m_lanterns.size(), m_tufts.size());
}

void CliffWorld::build_rock_bodies() {
    // The wall itself, as a column of static boxes. PhysicsWorld2D has boxes and
    // circles only, so a curved silhouette becomes a staircase; at a 1.2-unit
    // step the player cannot tell, and the drawn rock follows the exact profile
    // underneath.
    constexpr f32 kStep = 1.2f;
    for (f32 y = kCliffTopY - 6.0f; y < kCliffBottomY + 4.0f; y += kStep) {
        const f32 face = cliff_face_x(y);
        Body2D body;
        body.shape = scene2d::Shape2D::box((kCliffRightX - face) * 0.5f, kStep * 0.5f);
        // Centred on the midpoint of the step's slab.
        body.position = Vec2{face + body.shape.half.x, y + kStep * 0.5f};
        body.is_static = true;
        body.friction = 0.9f;
        body.restitution = 0.0f;
        m_phys.create_body(body);
    }
}

void CliffWorld::build_ledges() {
    // The valley floor: wide, flat, and where the story starts.
    {
        Ledge base;
        base.y = kCliffBottomY - 2.0f;
        base.h = 2.0f;
        base.x = -16.0f;
        base.w = cliff_face_x(base.y) - base.x;
        Body2D body;
        body.shape = scene2d::Shape2D::box(base.w * 0.5f, base.h * 0.5f);
        body.position = Vec2{base.x + base.w * 0.5f, base.y + base.h * 0.5f};
        body.is_static = true;
        body.friction = 0.95f;
        body.restitution = 0.0f;
        base.body = m_phys.create_body(body);
        m_ledges.push_back(base);
    }

    // Climb from the floor to the summit platform. The shelf x is seeded from the
    // previous ledge and then pulled toward the wall, so the path stays
    // reachable while the silhouette never looks like a staircase.
    f32 y = kCliffBottomY - 4.4f;
    f32 shelf_x = -6.0f;
    u32 counter = 0;

    while (y > kCliffTopY + 5.0f) {
        const f32 r0 = hash01(kSeed + counter * 7u + 1u);
        const f32 r1 = hash01(kSeed + counter * 7u + 2u);
        const f32 r2 = hash01(kSeed + counter * 7u + 3u);
        const f32 r3 = hash01(kSeed + counter * 7u + 4u);
        ++counter;

        Ledge ledge;
        ledge.y = y;
        ledge.h = 1.05f;
        ledge.w = kLedgeWidthMin + r0 * (kLedgeWidthMax - kLedgeWidthMin);

        // Reach: how far this ledge steps sideways from the one below.
        f32 reach = (r1 * 2.0f - 1.0f) * kLedgeReachMax;
        // Every few ledges, force a decisive sideways move so the climb has
        // shape instead of drifting.
        if (counter % 5 == 0) reach = (r1 > 0.5f ? 1.0f : -1.0f) * (kLedgeReachMax * 0.85f);
        f32 x = shelf_x + reach;

        // The ledge must end flush with the rock face so there is no seam to
        // fall through, and must not be pushed off the left edge of the world.
        const f32 face = cliff_face_x(y);
        if (x + ledge.w > face) x = face - ledge.w;
        if (x < kCliffLeftX + 3.0f) x = kCliffLeftX + 3.0f;

        ledge.x = x;
        shelf_x = x + ledge.w * 0.5f;

        // Every fourth ledge slides. Sliders are the game's one moving element,
        // and they are placed where the climb would otherwise be monotonous.
        ledge.moving = (counter % 4 == 2);
        if (ledge.moving) {
            ledge.travel = 1.4f + r2 * 1.5f;
            ledge.speed = 0.55f + r3 * 0.5f;
            ledge.phase = r0 * 6.2831853f;
        }

        Body2D body;
        body.shape = scene2d::Shape2D::box(ledge.w * 0.5f, ledge.h * 0.5f);
        body.position = Vec2{ledge.x + ledge.w * 0.5f, ledge.y + ledge.h * 0.5f};
        body.is_static = true;
        body.friction = 0.95f;
        body.restitution = 0.0f;
        ledge.body = m_phys.create_body(body);
        m_ledges.push_back(ledge);

        y -= kLedgeGapMin + r1 * (kLedgeGapMax - kLedgeGapMin);
    }

    // The summit shelf: broad and flat, so the shrine has somewhere to stand.
    {
        Ledge top;
        top.y = kCliffTopY + 3.0f;
        top.h = 1.6f;
        top.x = -7.0f;
        top.w = cliff_face_x(top.y) - top.x;
        Body2D body;
        body.shape = scene2d::Shape2D::box(top.w * 0.5f, top.h * 0.5f);
        body.position = Vec2{top.x + top.w * 0.5f, top.y + top.h * 0.5f};
        body.is_static = true;
        body.friction = 0.95f;
        body.restitution = 0.0f;
        top.body = m_phys.create_body(body);
        m_ledges.push_back(top);

        m_shrine = Vec2{top.x + 3.4f, top.y - 1.2f};
    }
}

void CliffWorld::build_contents() {
    u32 counter = 0;
    for (const Ledge& ledge : m_ledges) {
        if (ledge.w < 3.0f && ledge.y < kCliffTopY + 6.0f) continue;

        const f32 cx = ledge.x + ledge.w * 0.5f;

        // A flame on most ledges — the reward for reaching it at all.
        if (hash01(kSeed + counter * 13u + 5u) > 0.28f && ledge.w > 3.0f) {
            Flame flame;
            flame.pos = Vec2{cx + (hash01(kSeed + counter * 13u + 6u) - 0.5f) * ledge.w * 0.4f,
                             ledge.y - 1.5f};
            flame.phase = hash01(kSeed + counter * 13u + 7u) * 6.2831853f;
            m_flames.push_back(flame);
        }

        // Ivy rooted along the top edge. T_VineLeaf.png is a 512px sheet of
        // leaves on transparent ground, so each tuft takes one cell of it.
        const int tuft_count = 1 + static_cast<int>(hash01(kSeed + counter * 13u + 8u) * 3.0f);
        for (int i = 0; i < tuft_count; ++i) {
            Tuft tuft;
            const f32 t = static_cast<f32>(i + 1) / static_cast<f32>(tuft_count + 1);
            tuft.pos = Vec2{ledge.x + ledge.w * t, ledge.y};
            tuft.size = 0.7f + hash01(kSeed + counter * 31u + i) * 0.85f;
            tuft.rotation = -22.0f + hash01(kSeed + counter * 31u + i + 7u) * 44.0f;
            // 4x4 cells over the 512px sheet: 16 distinct leaves.
            tuft.cell = static_cast<u32>(hash01(kSeed + counter * 31u + i + 13u) * 15.0f) % 16u;
            tuft.parallax = 1.0f;
            m_tufts.push_back(tuft);
        }

        // Spikes, but never on a ledge the player has to cross to continue, and
        // never within a body's width of either end — a hazard you cannot dodge
        // is just a wall with better art.
        if (ledge.w > 4.2f && ledge.y > kCliffTopY + 14.0f &&
            hash01(kSeed + counter * 13u + 9u) > 0.62f) {
            Spike spike;
            const f32 sw = 1.1f;
            spike.w = sw;
            spike.x = ledge.x + ledge.w * (0.35f + hash01(kSeed + counter * 13u + 10u) * 0.3f);
            spike.y = ledge.y;
            m_spikes.push_back(spike);
        }

        ++counter;
    }

    // Shrine lanterns every ~26 units of climb: enough to make the respawn
    // point move with progress, sparse enough that lighting one is an event.
    for (f32 y = kCliffBottomY - 20.0f; y > kCliffTopY + 10.0f; y -= 26.0f) {
        // Find the nearest ledge at that height and hang the lantern beside it.
        const Ledge* best = nullptr;
        f32 best_d = 1e9f;
        for (const Ledge& ledge : m_ledges) {
            const f32 d = std::fabs(ledge.y - y);
            if (d < best_d) {
                best_d = d;
                best = &ledge;
            }
        }
        if (!best) continue;
        Lantern lantern;
        lantern.pos = Vec2{best->x + best->w * 0.5f, best->y - 1.3f};
        m_lanterns.push_back(lantern);
    }

    // Four bats patrolling the middle third of the cliff, where there is enough
    // open air for their sweep to read as a threat.
    for (int i = 0; i < 4; ++i) {
        const u32 s = kSeed + 0xB00u + static_cast<u32>(i) * 97u;
        Bat bat;
        bat.home = Vec2{-4.0f + hash01(s) * 4.0f,
                        kCliffBottomY - 30.0f - hash01(s + 1u) * 66.0f};
        bat.range = 4.5f + hash01(s + 2u) * 3.5f;
        bat.speed = 0.8f + hash01(s + 3u) * 0.7f;
        bat.phase = hash01(s + 4u) * 6.2831853f;
        bat.pos = bat.home;
        m_bats.push_back(bat);
    }
}

BodyHandle CliffWorld::create_player(Vec2 position) {
    Body2D body;
    body.shape = scene2d::Shape2D::box(kPlayerHalf.x, kPlayerHalf.y);
    body.position = position;
    body.density = 1.0f;
    body.friction = 0.05f;   // Near-frictionless: the ledge tops are what stop
                             // the player, not lateral scraping.
    body.restitution = 0.0f;  // No bounce. A bouncy climber reads as a bug.
    body.linear_damping = 0.0f;
    body.fixed_rotation = true;  // The player never tumbles; gravity does not
                                 // need angular response to walk up a cliff.
    return m_phys.create_body(body);
}

void CliffWorld::step_movers(f32 t) {
    for (Ledge& ledge : m_ledges) {
        if (!ledge.moving || !ledge.body.valid()) continue;
        ledge.offset = std::sin(t * ledge.speed + ledge.phase) * ledge.travel;
        if (Body2D* body = m_phys.body(ledge.body)) {
            body->position.x = ledge.x + ledge.w * 0.5f + ledge.offset;
        }
    }
}

bool CliffWorld::grounded(BodyHandle handle) const {
    if (!handle.valid()) return false;
    for (const scene2d::ContactManifold& contact : m_phys.contacts()) {
        if (contact.is_trigger) continue;

        // The manifold normal runs from A to B. What matters is the direction
        // *from the player towards the other body*, which flips with the pair
        // order, and y grows downward — so a ground contact points +y.
        Vec2 away_from_player;
        if (contact.body_a == handle.index) {
            away_from_player = contact.normal;
        } else if (contact.body_b == handle.index) {
            away_from_player = -contact.normal;
        } else {
            continue;
        }
        if (away_from_player.y > 0.5f) return true;
    }
    return false;
}

void CliffWorld::step_motes(f32 dt, const Vec2& camera_pos, f32 view_w, f32 view_h) {
    // Wrap motes around the camera instead of respawning them: dust that never
    // repeats and never dies reads as continuous air, and the field costs one
    // loop over a fixed array.
    const f32 margin = 2.0f;
    for (Mote& mote : m_motes) {
        mote.pos += mote.vel * dt;
        mote.life -= dt;

        const f32 half_w = view_w * 0.5f + margin;
        const f32 half_h = view_h * 0.5f + margin;
        if (mote.pos.x < camera_pos.x - half_w) mote.pos.x = camera_pos.x + half_w;
        if (mote.pos.x > camera_pos.x + half_w) mote.pos.x = camera_pos.x - half_w;
        if (mote.pos.y < camera_pos.y - half_h) mote.pos.y = camera_pos.y + half_h;
        if (mote.pos.y > camera_pos.y + half_h) mote.pos.y = camera_pos.y - half_h;

        if (mote.life <= 0.0f) mote.life = mote.max_life;
    }
}

} // namespace nf::cliff