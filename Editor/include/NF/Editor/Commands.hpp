#pragma once

// NF/Editor/Commands.hpp — undoable scene edit commands.
//
// Every command stores plain data (Entity id + generation, component values)
// and resolves live handles from the World on each call, so undo/redo never
// holds raw pointers or dangling references. A generation mismatch means the
// entity died and was recycled: the command becomes a safe no-op.
//
// Delete/undo-delete restores a functionally identical entity (same name and
// components, children re-attached). It cannot resurrect the original handle:
// callers must re-resolve identity via Selection::prune() after structural ops.

#include <NF/Assets/AssetId.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::runtime {
class Runtime;
} // namespace nf::runtime

namespace nf::editor {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void apply(ecs::World& world) = 0;
    virtual void undo(ecs::World& world) = 0;
    virtual std::string label() const = 0;
    virtual ecs::Entity target() const { return ecs::kInvalidEntity; }
};

class CommandStack {
public:
    CommandStack() = default;

    // Applies the command immediately and pushes it (clears the redo stack).
    void push(std::unique_ptr<ICommand> cmd, ecs::World& world);
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    bool undo(ecs::World& world);
    bool redo(ecs::World& world);
    void clear();
    size_t undo_size() const { return m_undo.size(); }
    size_t redo_size() const { return m_redo.size(); }
    ecs::Entity last_target() const;
    std::string undo_label() const;
    std::string redo_label() const;

private:
    std::vector<std::unique_ptr<ICommand>> m_undo;
    std::vector<std::unique_ptr<ICommand>> m_redo;
};

// Creates an empty entity (Transform + Name, optional parent).
class CreateEntityCommand : public ICommand {
public:
    CreateEntityCommand(std::string name, ecs::Entity parent = ecs::kInvalidEntity);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_created; }

private:
    std::string m_name;
    ecs::Entity m_parent;
    ecs::Entity m_created = ecs::kInvalidEntity;
};

// Deletes an entity, snapshotting its components and child links.
// Undo restores an identical entity (possibly under a new handle).
class DeleteEntityCommand : public ICommand {
public:
    // Captures the snapshot; returns nullptr (with err) when e is not alive.
    static std::unique_ptr<DeleteEntityCommand> capture(ecs::World& world, ecs::Entity e,
                                                        std::string& out_err);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_restored; }

private:
    DeleteEntityCommand() = default;
    ecs::Entity m_original = ecs::kInvalidEntity;
    ecs::Entity m_restored = ecs::kInvalidEntity;
    bool m_had_transform = false;
    scene::Transform m_transform{};
    bool m_had_name = false;
    scene::NameComponent m_name{};
    bool m_had_mesh = false;
    runtime::MeshComponent m_mesh{};
    bool m_had_camera = false;
    runtime::CameraComponent m_camera{};
    bool m_had_light = false;
    runtime::DirectionalLight m_light{};
    bool m_had_prefab = false;
    scene::PrefabLinkComponent m_prefab{};
    ecs::Entity m_parent = ecs::kInvalidEntity;
    std::vector<ecs::Entity> m_children;
};

// Renames an entity (empty new name clears back to the generated label).
class RenameCommand : public ICommand {
public:
    RenameCommand(ecs::Entity e, std::string new_name);
    // Captures old name from the world; returns false when e is not alive.
    bool capture_old(ecs::World& world);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    std::string m_new_name;
    bool m_had_old = false;
    std::string m_old_name;
    bool m_old_captured = false;
};

// Replaces local position/rotation/scale (degrees / unit scale).
class SetTransformCommand : public ICommand {
public:
    SetTransformCommand(ecs::Entity e, const scene::Transform& before, const scene::Transform& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }
    // Writes the 9 TRS fields + dirty. Shared with SetTransformsCommand: the
    // multi-entity gizmo transaction applies the same write to every entry.
    static void write_pos_rot_scale(ecs::World& world, ecs::Entity e, const scene::Transform& v);

private:
    ecs::Entity m_entity;
    scene::Transform m_before{};
    scene::Transform m_after{};
};

// Moves/rotates/scales several entities as ONE undo step — the multi-select
// gizmo transaction. Each entry is a before/after Transform pair; an entry
// whose entity died or lost its Transform is skipped at apply/undo time
// (never fatal: the undo must survive the user deleting one of five objects
// mid-gesture). Entries are independent local TRS overwrites, so order does
// not matter — no hierarchy write-back happens in v0.1.
class SetTransformsCommand : public ICommand {
public:
    struct Entry {
        ecs::Entity entity;
        scene::Transform before{};
        scene::Transform after{};
    };
    // `verb` becomes the undo label ("Move"/"Rotate"/"Scale"); the count is
    // appended here so callers stay free of string formatting.
    explicit SetTransformsCommand(std::vector<Entry> entries, std::string verb);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override { return m_label; }
    ecs::Entity target() const override {
        return m_entries.empty() ? ecs::kInvalidEntity : m_entries.front().entity;
    }
    size_t size() const { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
    std::string m_label;
};

// Changes hierarchy parent (cycle-safe via scene::set_parent).
class ReparentCommand : public ICommand {
public:
    ReparentCommand(ecs::Entity e, ecs::Entity old_parent, ecs::Entity new_parent);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    ecs::Entity m_old_parent;
    ecs::Entity m_new_parent;
};

// Sets (or replaces) the mesh reference of an existing entity.
class SetMeshCommand : public ICommand {
public:
    SetMeshCommand(ecs::Entity e, bool had_before, const runtime::MeshComponent& before,
                   const runtime::MeshComponent& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    bool m_had_before = false;
    runtime::MeshComponent m_before{};
    runtime::MeshComponent m_after{};
};

// Creates an entity carrying Transform + Name + MeshComponent (drag & drop).
class CreateMeshEntityCommand : public ICommand {
public:
    CreateMeshEntityCommand(std::string name, ecs::Entity parent, const runtime::MeshComponent& mesh);
    // Same command with a placement. The seed transform is written as part of
    // apply(), so "create and place" is ONE undo step — what the Create menu
    // needs (a ground plane belongs at the origin, not in a second edit a user
    // has to undo separately).
    CreateMeshEntityCommand(std::string name, ecs::Entity parent, const runtime::MeshComponent& mesh,
                            const scene::Transform& seed);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_created; }

private:
    std::string m_name;
    ecs::Entity m_parent;
    runtime::MeshComponent m_mesh{};
    bool m_has_seed = false;
    scene::Transform m_seed{};
    ecs::Entity m_created = ecs::kInvalidEntity;
};

// Sets (or replaces) the camera parameters of an existing entity.
class SetCameraCommand : public ICommand {
public:
    SetCameraCommand(ecs::Entity e, bool had_before, const runtime::CameraComponent& before,
                     const runtime::CameraComponent& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    bool m_had_before = false;
    runtime::CameraComponent m_before{};
    runtime::CameraComponent m_after{};
};

// Sets (or replaces) the directional light of an existing entity.
class SetLightCommand : public ICommand {
public:
    SetLightCommand(ecs::Entity e, bool had_before, const runtime::DirectionalLight& before,
                    const runtime::DirectionalLight& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    bool m_had_before = false;
    runtime::DirectionalLight m_before{};
    runtime::DirectionalLight m_after{};
};

// Sets (or adds) the procedural sky settings of an existing entity.
class SetSkyCommand : public ICommand {
public:
    SetSkyCommand(ecs::Entity e, bool had_before, const runtime::SkyComponent& before,
                  const runtime::SkyComponent& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    bool m_had_before = false;
    runtime::SkyComponent m_before{};
    runtime::SkyComponent m_after{};
};

// Reassigns an entity's mesh material path (shared asset, not a copy).
// The entity must carry a MeshComponent; validated by the factory.
class SetMaterialAssignmentCommand : public ICommand {
public:
    SetMaterialAssignmentCommand(ecs::Entity e, std::string before_path, std::string after_path);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    std::string m_before;
    std::string m_after;
};

// Sets (or replaces) an entity's prefab link (instance root tagging).
class SetPrefabLinkCommand : public ICommand {
public:
    SetPrefabLinkCommand(ecs::Entity e, bool had_before, const scene::PrefabLinkComponent& before,
                         const scene::PrefabLinkComponent& after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_entity; }

private:
    ecs::Entity m_entity;
    bool m_had_before = false;
    scene::PrefabLinkComponent m_before{};
    scene::PrefabLinkComponent m_after{};
};

// Destroys a whole subtree (root + descendants), snapshotting every entity.
// Undo recreates the subtree with remapped links (fresh handles, same shape).
class DeleteSubtreeCommand : public ICommand {
public:
    static std::unique_ptr<DeleteSubtreeCommand> capture(ecs::World& world, ecs::Entity root,
                                                         std::string& out_err);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override { return m_restored_root; }

private:
    DeleteSubtreeCommand() = default;
    struct Entry {
        ecs::Entity original;
        bool had_transform = false;
        scene::Transform transform{};
        bool had_name = false;
        scene::NameComponent name{};
        bool had_mesh = false;
        runtime::MeshComponent mesh{};
        bool had_camera = false;
        runtime::CameraComponent camera{};
        bool had_light = false;
        runtime::DirectionalLight light{};
        bool had_prefab = false;
        scene::PrefabLinkComponent prefab{};
        ecs::Entity orig_parent;
        ecs::Entity restored;
    };
    std::vector<Entry> m_entries; // root-first order
    ecs::Entity m_restored_root;
};

// Instantiates a prefab template Scene (pinned at construction) under a
// parent, tagging transplanted roots with the template path. Redo re-clones
// from the pinned copy, so redo never sees later file edits (documented;
// Revert pulls fresh bytes explicitly).
class InstantiatePrefabCommand : public ICommand {
public:
    InstantiatePrefabCommand(scene::Scene prefab_template, std::string prefab_path,
                             ecs::Entity parent);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override {
        return m_created.empty() ? ecs::kInvalidEntity : m_created.front();
    }

private:
    scene::Scene m_template;
    std::string m_path;
    ecs::Entity m_parent;
    std::vector<ecs::Entity> m_created;
};

// Revert = delete instance subtree + instantiate fresh from file, as ONE
// undo step (composed from the two commands above).
class RevertPrefabCommand : public ICommand {
public:
    RevertPrefabCommand(std::unique_ptr<DeleteSubtreeCommand> del,
                        std::unique_ptr<InstantiatePrefabCommand> inst);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    ecs::Entity target() const override;

private:
    std::unique_ptr<DeleteSubtreeCommand> m_del;
    std::unique_ptr<InstantiatePrefabCommand> m_inst;
};

// Binds (or, when empty, unbinds) a shared material's albedo texture. Like
// parameters, the binding lives in the Runtime; apply/undo write through.
// Not entity-bound: target() is invalid by design.
class SetMaterialAlbedoCommand : public ICommand {
public:
    SetMaterialAlbedoCommand(runtime::Runtime* runtime, std::string material_path,
                             std::string before_tex, std::string after_tex);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;

private:
    runtime::Runtime* m_runtime = nullptr;
    std::string m_material;
    std::string m_before;
    std::string m_after;
};

// Switches a shared material's sampler mip filter (None/Nearest/Linear).
// Rebinds the cached sampler, never re-uploads. Like albedo, the state
// lives in the Runtime; apply/undo write through. Not entity-bound.
class SetMaterialMipModeCommand : public ICommand {
public:
    SetMaterialMipModeCommand(runtime::Runtime* runtime, std::string material_path,
                              rhi::MipMapMode before_mode, rhi::MipMapMode after_mode);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;

private:
    bool write_through(rhi::MipMapMode mode);
    runtime::Runtime* m_runtime = nullptr;
    std::string m_material;
    rhi::MipMapMode m_before = rhi::MipMapMode::Linear;
    rhi::MipMapMode m_after = rhi::MipMapMode::Linear;
};

// Edits a shared material asset's parameters. The values live in the
// Runtime's renderer instance (one UBO per path), so apply/undo write through
// the non-owning Runtime pointer captured at construction; the World is
// untouched. Not entity-bound: target() is invalid by design.
class SetMaterialParamsCommand : public ICommand {
public:
    SetMaterialParamsCommand(runtime::Runtime* runtime, std::string path,
                             rendering::PBRMaterialParams before,
                             rendering::PBRMaterialParams after);
    void apply(ecs::World& world) override;
    void undo(ecs::World& world) override;
    std::string label() const override;
    const std::string& path() const { return m_path; }

private:
    bool write_through(const rendering::PBRMaterialParams& params);
    runtime::Runtime* m_runtime = nullptr;
    std::string m_path;
    rendering::PBRMaterialParams m_before{};
    rendering::PBRMaterialParams m_after{};
};

} // namespace nf::editor
