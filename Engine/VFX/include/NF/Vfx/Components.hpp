#pragma once

// NF/Vfx/Components.hpp — per-entity particle emitter authoring (Phase 25).
//
// ParticleSystem is the simulation; this component is the scene's description
// of one emitter: its configuration plus an enable switch. The Runtime builds
// one ParticleSystem per enabled component on scene adopt and steps it every
// frame, so a scene that declares emitters actually emits — before this, the
// whole VFX module was reachable only from tests. The live particle array is
// a Runtime artefact (like a fracture asset): the scene persists the config,
// never the particles.

#include <NF/Vfx/Particles.hpp>

namespace nf::vfx {

struct ParticleComponent {
    EmitterConfig config;
    bool enabled = true;
};

} // namespace nf::vfx
