#pragma once

// NF/Editor/PlayMode.hpp — edit/play isolation for v0.1 (no scripting).
//
// play() snapshots the edit scene with clone_scene(). While playing, the
// shell disables structural edits, so stop() can always restore the exact
// pre-play edit world: nothing the user did not save is ever lost.
// clone_scene() remaps parent links, so hierarchy survives the copy.

#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Scene.hpp>

#include <memory>
#include <optional>
#include <string>

namespace nf::editor {

// Deep-copies name/metadata and all v0.1 components (Transform with parent
// remap, Name, Mesh, Camera, Light).
std::unique_ptr<scene::Scene> clone_scene(const scene::Scene& src);

// Structural comparison ignoring entity ids (matches by Name, falling back to
// component-set signature). Used by tests and the stop() integrity check.
bool scenes_equal_structure(const scene::Scene& a, const scene::Scene& b, std::string& out_diff);

std::optional<ecs::Entity> find_by_name(const scene::Scene& scene, const std::string& name);

class PlaySession {
public:
    PlaySession() = default;

    bool playing() const { return m_playing; }
    // Snapshots edit for a play session. Returns false with err when empty.
    bool play(const scene::Scene& edit, std::string& out_err);
    // Ends the session; the edit scene itself is never mutated by play/stop.
    bool stop(std::string& out_err);
    const scene::Scene* snapshot() const { return m_snapshot.get(); }

private:
    bool m_playing = false;
    std::unique_ptr<scene::Scene> m_snapshot;
};

} // namespace nf::editor
