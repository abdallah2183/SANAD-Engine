#include <NF/Editor/SceneCopy.hpp>

#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Components.hpp>

namespace nf::editor {

void copy_entity_components(const ecs::World& src, ecs::Entity from, ecs::World& dst,
                            ecs::Entity to) {
    if (const auto* t = src.get<scene::Transform>(from)) {
        dst.add<scene::Transform>(to, *t);
    }
    if (const auto* n = src.get<scene::NameComponent>(from)) {
        dst.add<scene::NameComponent>(to, *n);
    }
    if (const auto* m = src.get<runtime::MeshComponent>(from)) {
        dst.add<runtime::MeshComponent>(to, *m);
    }
    if (const auto* c = src.get<runtime::CameraComponent>(from)) {
        dst.add<runtime::CameraComponent>(to, *c);
    }
    if (const auto* l = src.get<runtime::DirectionalLight>(from)) {
        dst.add<runtime::DirectionalLight>(to, *l);
    }
    if (const auto* pl = src.get<runtime::PointLightComponent>(from)) {
        dst.add<runtime::PointLightComponent>(to, *pl);
    }
    if (const auto* sl = src.get<runtime::SpotLightComponent>(from)) {
        dst.add<runtime::SpotLightComponent>(to, *sl);
    }
    if (const auto* s = src.get<runtime::SkyComponent>(from)) {
        dst.add<runtime::SkyComponent>(to, *s);
    }
    if (const auto* pp = src.get<runtime::PostProcessComponent>(from)) {
        dst.add<runtime::PostProcessComponent>(to, *pp);
    }
    // Phase 59: the feedback component was missing from this list, so a copy of
    // an entity (duplicate, prefab) silently lost its authored combat feedback
    // and the copy fired into the world with no flash, tracer, decal or sound.
    if (const auto* fb = src.get<runtime::FeedbackComponent>(from)) {
        dst.add<runtime::FeedbackComponent>(to, *fb);
    }
    // Phase 60: senses travel with the entity, like a faction or a weapon does.
    if (const auto* pc = src.get<runtime::PerceptionComponent>(from)) {
        dst.add<runtime::PerceptionComponent>(to, *pc);
    }
    // Phase 61: the considerations travel with the entity too.
    if (const auto* brain = src.get<runtime::BrainComponent>(from)) {
        dst.add<runtime::BrainComponent>(to, *brain);
    }
    if (const auto* machine = src.get<runtime::StateMachineComponent>(from)) {
        dst.add<runtime::StateMachineComponent>(to, *machine);
    }
    if (const auto* p = src.get<scene::PrefabLinkComponent>(from)) {
        dst.add<scene::PrefabLinkComponent>(to, *p);
    }
    if (const auto* rb = src.get<physics::RigidBodyComponent>(from)) {
        physics::RigidBodyComponent fresh = *rb;
        fresh.body = physics::BodyHandle{};
        dst.add<physics::RigidBodyComponent>(to, fresh);
    }
    if (const auto* col = src.get<physics::ColliderComponent>(from)) {
        dst.add<physics::ColliderComponent>(to, *col);
    }
    if (const auto* d = src.get<runtime::DestructibleComponent>(from)) {
        dst.add<runtime::DestructibleComponent>(to, *d);
    }
    if (const auto* a = src.get<animation::AnimationComponent>(from)) {
        dst.add<animation::AnimationComponent>(to, *a);
    }
    if (const auto* au = src.get<audio::AudioComponent>(from)) {
        dst.add<audio::AudioComponent>(to, *au);
    }
    // The audio environment and the day/night cycle. The restore is what
    // brings the scene back after a play session, so an entry that is not
    // copied here is a component the user LOSES by pressing Play and Stop:
    // the reverb zone, the music, the ambience and the clock all vanished
    // from the scene the moment the session ended.
    if (const auto* rz = src.get<audio::ReverbZoneComponent>(from)) {
        dst.add<audio::ReverbZoneComponent>(to, *rz);
    }
    if (const auto* mu = src.get<audio::MusicComponent>(from)) {
        dst.add<audio::MusicComponent>(to, *mu);
    }
    if (const auto* am = src.get<audio::AmbienceComponent>(from)) {
        dst.add<audio::AmbienceComponent>(to, *am);
    }
    if (const auto* tod = src.get<runtime::TimeOfDayComponent>(from)) {
        dst.add<runtime::TimeOfDayComponent>(to, *tod);
    }
    if (const auto* g = src.get<gameplay::GameplayModuleComponent>(from)) {
        dst.add<gameplay::GameplayModuleComponent>(to, *g);
    }
    if (const auto* sc = src.get<scripting::ScriptComponent>(from)) {
        dst.add<scripting::ScriptComponent>(to, *sc);
    }
    if (const auto* pc = src.get<vfx::ParticleComponent>(from)) {
        dst.add<vfx::ParticleComponent>(to, *pc);
    }
    if (const auto* cc = src.get<physics::ClothComponent>(from)) {
        dst.add<physics::ClothComponent>(to, *cc);
    }
    if (const auto* ch = src.get<physics::CharacterComponent>(from)) {
        physics::CharacterComponent fresh = *ch;
        fresh.wish_dir = Vec3{0.0f, 0.0f, 0.0f};
        fresh.jump = false;
        fresh.grounded = false;
        dst.add<physics::CharacterComponent>(to, fresh);
    }
    if (const auto* nm = src.get<runtime::NavMeshComponent>(from)) {
        dst.add<runtime::NavMeshComponent>(to, *nm);
    }
    if (const auto* na = src.get<runtime::NavAgentComponent>(from)) {
        dst.add<runtime::NavAgentComponent>(to, *na);
    }
    if (const auto* terrain = src.get<runtime::TerrainComponent>(from)) {
        dst.add<runtime::TerrainComponent>(to, *terrain);
    }
    if (const auto* route = src.get<runtime::NavRouteComponent>(from)) {
        dst.add<runtime::NavRouteComponent>(to, *route);
    }
    if (const auto* vc = src.get<physics::VehicleComponent>(from)) {
        // Tune crosses, live state does not. Without this, a duplicate or a Stop
        // restore would hold a JoltVehicle view into a world that is gone.
        physics::VehicleComponent fresh = *vc;
        fresh.throttle = 0.0f;
        fresh.steer = 0.0f;
        fresh.brake = 0.0f;
        fresh.handbrake = 0.0f;
        fresh.vehicle = nullptr;
        dst.add<physics::VehicleComponent>(to, fresh);
    }
}

} // namespace nf::editor
