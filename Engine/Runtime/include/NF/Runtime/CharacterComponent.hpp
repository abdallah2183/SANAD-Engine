#pragma once

// NF/Runtime/CharacterComponent.hpp — a character: a skinned mesh, a
// skeleton, and the clips that drive it.
//
// The engine's animation pipeline already knows how to SAMPLE a rig
// (skeleton, clips, state machine) and how to turn a pose into a skin
// palette. What was missing was the last step -- the palette has to reach
// the GPU -- and the source of the rig: a glTF file with a skin and
// animations.
//
// The character is skinned on the CPU. The palette is one matrix per bone,
// and a vertex is a weighted sum of the four bones it is bound to. For a
// character of a few hundred vertices that is a rounding error next to the
// draw call, and it costs the renderer nothing: the deformed vertices go
// into the same vertex buffer every other mesh uses.
//
// The alternative -- a GPU skinning shader and a second vertex format -- is
// a renderer-wide change, and the engine's own mesh format is deliberately
// single-purpose: the project has already been bitten by two incompatible
// .nfmesh formats. A CPU-skinned character needs no new format at all.

#include <NF/Animation/AnimationClip.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Rendering/Handles.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <string>
#include <vector>

namespace nf::runtime {

/// The character, as the scene carries it and as the runtime fills it in.
struct CharacterComponent {
    /// The glTF that carries the mesh, the skin and the clips. A logical
    /// path, so it resolves through the VFS like every other asset.
    std::string source;

    /// The bind-pose vertices: the deformed ones are computed from these,
    /// every frame, from the palette.
    std::vector<rendering::Vertex> base_vertices;

    /// Four joint indices per vertex, in the skin's joint order.
    std::vector<u16> joints;

    /// Four weights per vertex, summing to one.
    std::vector<float> weights;

    /// The inverse bind matrices, from the file. The file's, not the
    /// skeleton's rest pose: a rig whose bind pose differs from its rest
    /// pose is legal, and using the wrong one bends the character.
    std::vector<Mat4> inverse_bind;

    /// The character's OWN mesh. A skinned character deforms differently
    /// every frame, so it cannot share the mesh the library holds for an
    /// ordinary asset: two characters of the same file would fight over one
    /// vertex buffer.
    rendering::StaticMeshHandle mesh;

    /// Whether the load succeeded. A character that failed to load draws
    /// nothing and says why, once.
    bool loaded = false;
    std::string error;

    /// Where the character was last frame, for the speed the locomotion
    /// state machine reads.
    Vec3 last_position{0.0f, 0.0f, 0.0f};
    bool has_last_position = false;
};

/// The names the locomotion state machine looks for. A file that calls its
/// clips "Idle"/"Walk"/"Run" (the usual) or "idle"/"walk"/"run" both work:
/// the comparison is case-insensitive.
inline constexpr const char* kCharacterIdleClip = "idle";
inline constexpr const char* kCharacterWalkClip = "walk";
inline constexpr const char* kCharacterRunClip = "run";

/// The state machine parameter the speed drives.
inline constexpr const char* kCharacterSpeedParam = "speed";

/// The speed (metres per second) below which the character is considered
/// standing still.
inline constexpr float kCharacterWalkThreshold = 0.15f;

/// The speed above which the character is running.
inline constexpr float kCharacterRunThreshold = 3.0f;

/// True when two clip names are the same clip, ignoring case.
inline bool character_clip_is(const std::string& name, const char* wanted);

} // namespace nf::runtime
