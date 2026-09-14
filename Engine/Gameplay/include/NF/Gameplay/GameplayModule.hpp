#pragma once

// NF/Gameplay/GameplayModule.hpp — the C++ gameplay layer (Phase 10, W2)
//
// Design doc §249 S0 asks for "a C++ gameplay module" with lifecycle hooks.
// This is that: a class with on_init / on_update / on_shutdown / on_scene_load /
// on_scene_unload, owned by the Runtime and driven once per frame.
//
// Scope note: this is *not* a scripting VM. S2 (C#/Lua), S3 (hot reload) and S4
// (visual scripting) are later phases. What ships here is the structured C++
// authoring path plus the reflection hook that lets a module's state be saved
// and edited without hand-written serialization.

#include <NF/Core/Reflection.hpp>
#include <NF/Core/Types.hpp>

#include <string_view>

namespace nf {

namespace ecs     { class World; }
namespace scene   { class Scene; }
namespace physics { class PhysicsWorld; }
namespace audio   { struct AudioBus; }

namespace gameplay {

/// Abstract input source.
///
/// `Engine/Input` is still a placeholder, so gameplay code talks to this instead
/// of a concrete device: the Runtime supplies whatever it has, and a module that
/// never reads input does not care. A headless run leaves it null, which is why
/// every consumer must check before use rather than assuming a device.
class IInputSource {
public:
    virtual ~IInputSource() = default;

    /// True while `action` is held. Unknown actions are false, not an error.
    [[nodiscard]] virtual bool action_pressed(std::string_view action) const = 0;
    /// Signed magnitude in [-1, 1] for an analogue action; 0 when absent.
    [[nodiscard]] virtual f32 action_axis(std::string_view action) const = 0;
};

/// Everything a module may touch, refreshed by the Runtime before each callback.
///
/// Every pointer is non-owning and may be null: a headless run has no input
/// source, and a scene with no physics has no world. Null-checking is the
/// module's job — the alternative is a callback that works in the editor and
/// crashes in a packaged game.
struct GameplayContext {
    ecs::World*            world         = nullptr;
    scene::Scene*          scene         = nullptr;
    physics::PhysicsWorld* physics       = nullptr;
    audio::AudioBus*       audio         = nullptr;
    IInputSource*          input         = nullptr;
    f32                    dt            = 0.0f;
    u64                    frame         = 0;
    u32                    scene_version = 0;

    [[nodiscard]] bool has_world() const { return world != nullptr; }
};

/// A module's reflected state block: the struct instance plus its metadata.
///
/// Modules derive from a polymorphic base, and `offsetof` is not usable on a
/// non-standard-layout type — so the state lives in a separate plain struct that
/// the module points at, rather than being reflected on the module itself. That
/// also keeps save/load and the inspector working on data instead of on a class
/// with virtuals.
struct GameplayStateBinding {
    void*            instance = nullptr;
    const ClassInfo* meta     = nullptr;

    [[nodiscard]] bool valid() const { return instance != nullptr && meta != nullptr; }
};

class GameplayModule {
public:
    virtual ~GameplayModule() = default;

    GameplayModule(const GameplayModule&) = delete;
    GameplayModule& operator=(const GameplayModule&) = delete;

    /// Stable identity. This is what a scene file stores, so renaming it
    /// invalidates saved scenes — treat it as a serialized field.
    [[nodiscard]] virtual const char* name() const = 0;

    /// Lower runs first; ties break on registration order, which the registry
    /// keeps sorted so the order does not depend on static-init timing.
    ///
    /// Cross-module calls should go through late-bound queries rather than
    /// cached pointers: two modules at the same priority may be constructed in
    /// either order.
    [[nodiscard]] virtual f32 update_priority() const { return 0.0f; }

    virtual void on_init(GameplayContext& /*ctx*/) {}
    virtual void on_update(GameplayContext& /*ctx*/) {}
    virtual void on_shutdown(GameplayContext& /*ctx*/) {}
    virtual void on_scene_load(GameplayContext& /*ctx*/) {}
    virtual void on_scene_unload(GameplayContext& /*ctx*/) {}

    /// Reflected state for save/load and the inspector. Default: none, which
    /// makes the module stateless from the serializer's point of view.
    ///
    /// Override the non-const overload only; the const one forwards to it.
    virtual GameplayStateBinding state() { return {}; }

    [[nodiscard]] GameplayStateBinding state() const {
        // Safe: the binding only ever hands out a pointer that callers read.
        return const_cast<GameplayModule*>(this)->state();
    }

protected:
    GameplayModule() = default;
};

} // namespace gameplay
} // namespace nf
