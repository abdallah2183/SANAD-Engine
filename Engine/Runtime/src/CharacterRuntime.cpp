// NF/Runtime/CharacterRuntime.cpp — the character: loading it and moving it.
//
// Two jobs, and they are the two ends of the same cable:
//
//   * load: a glTF with a skin and animations becomes a mesh, a skeleton,
//     the clips, and the skin data (the joints, the weights, the inverse
//     bind matrices).
//   * step: the animation's pose becomes a skin palette, the palette
//     deforms the bind-pose vertices, and the deformed vertices go to the
//     GPU.
//
// The deformation runs on the CPU. A palette is one matrix per bone and a
// vertex is a weighted sum of the four bones it is bound to, so a character
// of a few hundred vertices costs a rounding error next to its draw call --
// and the renderer needs no change at all: the deformed vertices go into the
// same vertex buffer every other mesh uses.

#include <NF/Runtime/CharacterComponent.hpp>

#include <NF/Animation/Components.hpp>
#include <NF/Animation/Skinning.hpp>
#include <NF/Assets/AnimationImport.hpp>
#include <NF/Assets/GltfImport.hpp>
#include <NF/Assets/MeshImport.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace nf::runtime {

bool character_clip_is(const std::string& name, const char* wanted) {
    if (name.size() != std::strlen(wanted)) {
        return false;
    }
    for (usize i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) !=
            std::tolower(static_cast<unsigned char>(wanted[i]))) {
            return false;
        }
    }
    return true;
}

namespace {

// The name of the clip the locomotion state machine uses for `wanted`, or
// an empty string when the file has no such clip.
std::string find_clip(const std::unordered_map<std::string, animation::AnimationClip>& clips,
                      const char* wanted) {
    for (const auto& entry : clips) {
        if (character_clip_is(entry.first, wanted)) {
            return entry.first;
        }
    }
    return {};
}

// The state's transition list: the speed decides which state the character
// is in, and the thresholds are the same ones the constants name.
std::vector<animation::Transition> locomotion_transitions(const std::string& idle,
                                                          const std::string& walk,
                                                          const std::string& run) {
    std::vector<animation::Transition> transitions;
    auto add = [&](const std::string& to, animation::ConditionOp op, float threshold) {
        animation::Transition t;
        t.target_state = to;
        t.fade_duration = 0.18f;
        animation::Condition c;
        c.param = kCharacterSpeedParam;
        c.op = op;
        c.threshold = threshold;
        t.conditions.push_back(c);
        transitions.push_back(t);
    };
    if (!idle.empty() && !walk.empty()) {
        add(walk, animation::ConditionOp::Greater, kCharacterWalkThreshold);
        add(idle, animation::ConditionOp::LessEqual, kCharacterWalkThreshold);
    }
    if (!walk.empty() && !run.empty()) {
        add(run, animation::ConditionOp::Greater, kCharacterRunThreshold);
        add(walk, animation::ConditionOp::LessEqual, kCharacterRunThreshold);
    }
    return transitions;
}

} // namespace

void Runtime::load_characters() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<CharacterComponent>()) {
        CharacterComponent* ch = world.get<CharacterComponent>(e);
        if (ch == nullptr || ch->source.empty() || ch->loaded) {
            continue;
        }
        if (ch->error.empty() == false) {
            continue; // already reported; do not report it again every frame
        }

        // The file resolves through the VFS, like every other asset.
        auto resolved = m_vfs.resolve(ch->source);
        if (!resolved.ok) {
            ch->error = "cannot resolve '" + ch->source + "': " + resolved.error;
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}",
                         ch->source.c_str(), ch->error.c_str());
            continue;
        }
        assets::GltfImportResult imported =
            assets::import_gltf_file(resolved.value.string());
        if (!imported.ok) {
            ch->error = imported.error;
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        if (imported.meshes.empty()) {
            ch->error = "the file carries no mesh";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }

        // The skin: the joints, the weights and the inverse bind matrices.
        int skin_index = -1;
        for (usize mi = 0; mi < imported.mesh_skins.size(); ++mi) {
            if (imported.mesh_skins[mi].skin_index >= 0) {
                skin_index = imported.mesh_skins[mi].skin_index;
                break;
            }
        }
        if (skin_index < 0 || static_cast<usize>(skin_index) >= imported.skins.size()) {
            ch->error = "the file carries no skin: a character needs one";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        const assets::GltfSkinInfo& skin = imported.skins[static_cast<usize>(skin_index)];
        const assets::GltfMeshSkin& mesh_skin = imported.mesh_skins[0];
        if (mesh_skin.skin_index != skin_index || mesh_skin.joints.empty() ||
            mesh_skin.weights.size() != mesh_skin.joints.size()) {
            ch->error = "the skin binding does not match the mesh";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        ch->joints = mesh_skin.joints;
        ch->weights = mesh_skin.weights;
        ch->inverse_bind.clear();
        for (usize j = 0; j < skin.inverse_bind_matrices.size() / 16; ++j) {
            Mat4 m{};
            // glTF stores the matrix column-major; the engine's Mat4 is
            // row-major, so the transpose is the conversion.
            for (usize col = 0; col < 4; ++col) {
                for (usize row = 0; row < 4; ++row) {
                    m.m[row][col] = skin.inverse_bind_matrices[j * 16 + col * 4 + row];
                }
            }
            ch->inverse_bind.push_back(m);
        }

        // The skeleton and the clips.
        auto* anim = world.get<animation::AnimationComponent>(e);
        if (anim == nullptr) {
            anim = &world.add<animation::AnimationComponent>(e, animation::AnimationComponent{});
        }
        std::string err;
        animation::Skeleton skeleton;
        if (!assets::make_skeleton(imported, skin_index, skeleton, err)) {
            ch->error = err.empty() ? "the skeleton could not be built" : err;
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        anim->skeleton = skeleton;
        anim->clips.clear();
        const std::vector<int> node_to_bone = assets::joint_node_to_bone(imported, skin_index);
        for (const assets::GltfAnimationInfo& clip_info : imported.animations) {
            animation::AnimationClip clip;
            if (!assets::make_clip(imported, clip_info, skeleton, node_to_bone, clip, err)) {
                // A clip that cannot target this skin is reported and
                // skipped: the character plays the clips it can.
                NF_LOG_WARN(LogCategory::Core, "Runtime: character '{}': clip '{}' was skipped: {}",
                            ch->source.c_str(), clip_info.name.c_str(), err.c_str());
                continue;
            }
            anim->clips[clip_info.name] = clip;
        }
        if (anim->clips.empty()) {
            ch->error = "the file carries no usable clip";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }

        // The locomotion state machine: the states are the clips the file
        // carries, and the speed decides which one is playing.
        const std::string idle = find_clip(anim->clips, kCharacterIdleClip);
        const std::string walk = find_clip(anim->clips, kCharacterWalkClip);
        const std::string run = find_clip(anim->clips, kCharacterRunClip);
        anim->state_machine = animation::AnimationStateMachine{};
        bool any_state = false;
        for (const std::string* name : {&idle, &walk, &run}) {
            if (name->empty()) {
                continue;
            }
            animation::AnimState state;
            state.name = *name;
            state.clip_name = *name;
            anim->state_machine.add_state(state);
            any_state = true;
        }
        if (any_state) {
            // The transitions hang off the states, so they are added after
            // every state exists.
            const std::vector<animation::Transition> transitions =
                locomotion_transitions(idle, walk, run);
            for (animation::AnimState& state : anim->state_machine.states()) {
                for (const animation::Transition& t : transitions) {
                    if (t.target_state == state.name) {
                        continue;
                    }
                    state.transitions.push_back(t);
                }
            }
            anim->use_state_machine = true;
            anim->state_machine.set_param(kCharacterSpeedParam, 0.0f);
        }

        // The mesh: the character's own copy, deformed every frame.
        if (!imported.meshes[0]) {
            ch->error = "the mesh could not be read";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        const assets::MeshAsset& asset = *imported.meshes[0];
        std::unique_ptr<rendering::StaticMesh> mesh =
            rendering::make_static_mesh(asset, ch->source);
        if (!mesh) {
            ch->error = "the mesh could not be converted";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        ch->base_vertices = mesh->lods().empty() ? std::vector<rendering::Vertex>{}
                                                  : mesh->lods()[0].vertices;
        if (!mesh->upload(m_device, /*dynamic_vertices=*/true)) {
            ch->error = "the mesh could not be uploaded";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        const rendering::StaticMeshHandle handle = m_mesh_library->add(std::move(mesh));
        if (!handle.valid()) {
            ch->error = "the mesh library refused the character";
            NF_LOG_ERROR(LogCategory::Core, "Runtime: character '{}': {}", ch->source.c_str(),
                         ch->error.c_str());
            continue;
        }
        ch->mesh = handle;
        // The character's mesh IS its mesh: an entity that declares a rig and
        // no mesh of its own gets one, so the scene does not have to repeat
        // what the file already says.
        rendering::MeshComponent* mc = world.get<rendering::MeshComponent>(e);
        if (mc == nullptr) {
            mc = &world.add<rendering::MeshComponent>(e, rendering::MeshComponent{});
        }
        mc->mesh = handle;
        // The MATERIAL comes from the scene, not from the file: the file's
        // material is not an asset the engine can load on its own, and a
        // render object with no material is not drawn at all -- the character
        // would be loaded, animated and invisible.
        if (const auto* scene_mesh = world.get<MeshComponent>(e)) {
            if (!scene_mesh->material.empty()) {
                mc->material = material_for_path(scene_mesh->material);
            }
        }
        ch->loaded = true;
        ++m_character_count;
        NF_LOG_INFO(LogCategory::Core,
                    "Runtime: character '{}' loaded ({} vertices, {} bones, {} clip(s))",
                    ch->source.c_str(), ch->base_vertices.size(), skeleton.bones.size(),
                    anim->clips.size());
    }
}

void Runtime::step_characters(float dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<CharacterComponent>()) {
        CharacterComponent* ch = world.get<CharacterComponent>(e);
        auto* anim = world.get<animation::AnimationComponent>(e);
        if (ch == nullptr || anim == nullptr || !ch->loaded || anim->skeleton.bones.empty()) {
            continue;
        }

        // The speed: how far the character moved since the last frame. It is
        // what decides whether the character is standing, walking or
        // running, and it is measured from the transform rather than from a
        // controller, so a character driven by a script, by physics or by
        // the player all drive it the same way.
        const scene::Transform* t = world.get<scene::Transform>(e);
        float speed = 0.0f;
        if (t != nullptr) {
            const Vec3 now{t->world_x, t->world_y, t->world_z};
            if (ch->has_last_position && dt > 0.0f) {
                speed = (now - ch->last_position).length() / dt;
            }
            ch->last_position = now;
            ch->has_last_position = true;
        }
        if (anim->use_state_machine) {
            anim->state_machine.set_param(kCharacterSpeedParam, speed);
        }

        // The palette: the pose times the inverse bind matrices.
        animation::compute_skin_palette(anim->skeleton, anim->last_world_pose, ch->inverse_bind,
                                        m_character_palette);

        // The deformation: a vertex is a weighted sum of the four bones it
        // is bound to. The bind-space position goes through the palette the
        // same way the skinning reference does: v' = v * palette.
        rendering::StaticMesh* mesh = m_mesh_library ? m_mesh_library->get(ch->mesh) : nullptr;
        if (mesh == nullptr || mesh->lods().empty()) {
            continue;
        }
        auto& lod = mesh->lods()[0];
        if (lod.vertices.size() != ch->base_vertices.size()) {
            continue; // the mesh was replaced under us; skip this frame
        }
        for (usize v = 0; v < lod.vertices.size(); ++v) {
            const rendering::Vertex& src = ch->base_vertices[v];
            rendering::Vertex& dst = lod.vertices[v];
            Vec3 pos{src.position[0], src.position[1], src.position[2]};
            Vec3 nrm{src.normal[0], src.normal[1], src.normal[2]};
            Vec3 out_pos{0.0f, 0.0f, 0.0f};
            Vec3 out_nrm{0.0f, 0.0f, 0.0f};
            float total = 0.0f;
            for (usize k = 0; k < 4; ++k) {
                const usize slot = v * 4 + k;
                if (slot >= ch->joints.size() || slot >= ch->weights.size()) {
                    break;
                }
                const float w = ch->weights[slot];
                if (w <= 0.0f) {
                    continue;
                }
                const usize joint = ch->joints[slot];
                if (joint >= m_character_palette.size()) {
                    continue;
                }
                const Mat4& m = m_character_palette[joint];
                out_pos = out_pos + m.transform_point(pos) * w;
                out_nrm = out_nrm + m.transform_direction(nrm) * w;
                total += w;
            }
            if (total > 1e-6f) {
                out_pos = out_pos / total;
                out_nrm = out_nrm / total;
                const float len = out_nrm.length();
                if (len > 1e-6f) {
                    out_nrm = out_nrm / len;
                }
            } else {
                out_pos = pos;
                out_nrm = nrm;
            }
            dst.position[0] = out_pos.x;
            dst.position[1] = out_pos.y;
            dst.position[2] = out_pos.z;
            dst.normal[0] = out_nrm.x;
            dst.normal[1] = out_nrm.y;
            dst.normal[2] = out_nrm.z;

            // How far the vertex moved from its bind pose. It is the
            // character's version of the animated-transform deviation: the
            // acceptance run has to be able to tell a walking character from
            // a character whose rig never moved.
            const Vec3 bind{src.position[0], src.position[1], src.position[2]};
            const float moved = (out_pos - bind).length();
            if (moved > m_character_max_deviation) {
                m_character_max_deviation = moved;
            }
        }

        // To the GPU. The vertex buffer is host-visible (the character was
        // uploaded as a dynamic mesh), so this is a memcpy rather than a
        // staged copy that waits for the queue.
        if (rhi::Buffer* vb = mesh->vertex_buffer(0)) {
            const usize bytes = lod.vertices.size() * sizeof(rendering::Vertex);
            vb->update(lod.vertices.data(), 0, bytes);
        }
    }
}

} // namespace nf::runtime
