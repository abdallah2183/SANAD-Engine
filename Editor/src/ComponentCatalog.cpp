#include <NF/Editor/ComponentCatalog.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/AddObjectCatalog.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/UI/Localization.hpp>

namespace nf::editor {
namespace {
// Legacy attachment helpers initialize component-specific defaults. Capture their
// result so the catalog records the whole addition as one undoable operation.
template<typename T> class AttachComponentCommand final : public ICommand {
public:
    AttachComponentCommand(ecs::Entity entity, const T& component) : entity(entity), value(component) {}
    void apply(ecs::World& world) override { if (world.is_alive(entity)) world.add<T>(entity, value); }
    void undo(ecs::World& world) override { if (world.is_alive(entity)) world.remove<T>(entity); }
    std::string label() const override { return "Add component"; }
    ecs::Entity target() const override { return entity; }
    void remap_entity(ecs::Entity old_entity, ecs::Entity new_entity) override {
        if (entity == old_entity) entity = new_entity;
    }
private:
    ecs::Entity entity;
    T value;
};
template<typename T> bool record_attachment(ecs::World& world, CommandStack& stack,
                                           ecs::Entity entity, size_t previous_steps, bool attached) {
    if (!attached) return false;
    if (stack.undo_size() != previous_steps) return true;
    const auto* component = world.get<T>(entity);
    if (!component) return false;
    stack.push(std::make_unique<AttachComponentCommand<T>>(entity, *component), world);
    return true;
}
} // namespace
bool has_component(const ecs::World& world, ecs::Entity entity, ComponentKind kind) {
    if (!world.is_alive(entity)) return false;
    switch (kind) {
    case ComponentKind::Camera: return world.has<runtime::CameraComponent>(entity);
    case ComponentKind::Sun: return world.has<runtime::DirectionalLight>(entity);
    case ComponentKind::Sky: return world.has<runtime::SkyComponent>(entity);
    case ComponentKind::PostProcess: return world.has<runtime::PostProcessComponent>(entity);
    case ComponentKind::Decal: return world.has<runtime::DecalComponent>(entity);
    case ComponentKind::Reverb: return world.has<audio::ReverbZoneComponent>(entity);
    case ComponentKind::Sprite: return world.has<runtime::SpriteComponent>(entity);
    case ComponentKind::Tilemap: return world.has<runtime::TilemapComponent>(entity);
    case ComponentKind::Body2D: return world.has<runtime::RigidBody2DComponent>(entity);
    case ComponentKind::Collider2D: return world.has<runtime::Collider2DComponent>(entity);
    case ComponentKind::Body: return world.has<physics::RigidBodyComponent>(entity);
    case ComponentKind::Collider: return world.has<physics::ColliderComponent>(entity);
    case ComponentKind::Destructible: return world.has<runtime::DestructibleComponent>(entity);
    case ComponentKind::Particles: return world.has<vfx::ParticleComponent>(entity);
    case ComponentKind::Cloth: return world.has<physics::ClothComponent>(entity);
    case ComponentKind::Character: return world.has<physics::CharacterComponent>(entity);
    case ComponentKind::Navmesh: return world.has<runtime::NavMeshComponent>(entity);
    case ComponentKind::NavAgent: return world.has<runtime::NavAgentComponent>(entity);
    case ComponentKind::NavRoute: return world.has<runtime::NavRouteComponent>(entity);
    case ComponentKind::Terrain: return world.has<runtime::TerrainComponent>(entity);
    case ComponentKind::Squad: return world.has<runtime::SquadComponent>(entity);
    case ComponentKind::Jammer: return world.has<runtime::JammerComponent>(entity);
    case ComponentKind::Vehicle: return world.has<physics::VehicleComponent>(entity);
    case ComponentKind::Cable: return world.has<runtime::CableComponent>(entity);
    case ComponentKind::Weather: return world.has<runtime::WeatherComponent>(entity);
    case ComponentKind::RuleTile: return world.has<runtime::RuleTileComponent>(entity);
    case ComponentKind::FlowField: return world.has<runtime::FlowFieldComponent>(entity);
    case ComponentKind::Talents: return world.has<runtime::TalentComponent>(entity);
    case ComponentKind::Ragdoll: return world.has<runtime::RagdollComponent>(entity);
    case ComponentKind::Inventory: return world.has<runtime::InventoryComponent>(entity);
    case ComponentKind::SurfaceAudio: return world.has<runtime::SurfaceAudioComponent>(entity);
    case ComponentKind::Weapon: return world.has<runtime::WeaponComponent>(entity);
    case ComponentKind::Health: return world.has<runtime::HealthComponent>(entity);
    case ComponentKind::CombatAgent: return world.has<runtime::CombatAgentComponent>(entity);
    case ComponentKind::Faction: return world.has<runtime::FactionComponent>(entity);
    }
    return false;
}

bool component_entry_matches(const ComponentEntry& entry, std::string_view query) {
    const auto hay = add_object_fold(std::string(entry.keywords) + " " +
        ui::tr_for_language(entry.label_key, ui::Language::English) + " " +
        ui::tr_for_language(entry.label_key, ui::Language::Arabic));
    const auto words = add_object_fold(query);
    size_t at = 0;
    while (at < words.size()) {
        at = words.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) break;
        const auto end = words.find_first_of(" \t\r\n", at);
        if (hay.find(words.substr(at, end - at)) == std::string::npos) return false;
        at = end;
    }
    return true;
}

bool EditorApp::add_component(ecs::Entity e, ComponentKind kind, std::string& error) {
    if (!require_editable(error)) return false;
    if (!world() || !world()->is_alive(e)) { error = "Entity is not alive"; return false; }
    if (has_component(*world(), e, kind)) { error = "Component is already attached"; return false; }
    const auto previous_steps = m_stack.undo_size();
    switch (kind) {
    case ComponentKind::Camera: return record_attachment<runtime::CameraComponent>(*world(), m_stack, e, previous_steps, set_camera(e, CameraEdit{}, error));
    case ComponentKind::Sun: return record_attachment<runtime::DirectionalLight>(*world(), m_stack, e, previous_steps, set_light(e, LightEdit{}, error));
    case ComponentKind::Sky: return record_attachment<runtime::SkyComponent>(*world(), m_stack, e, previous_steps, set_sky(e, SkyEdit{}, error));
    case ComponentKind::PostProcess: return record_attachment<runtime::PostProcessComponent>(*world(), m_stack, e, previous_steps, set_post_process(e, runtime::PostProcessComponent{}, error));
    case ComponentKind::Decal: return record_attachment<runtime::DecalComponent>(*world(), m_stack, e, previous_steps, set_decal(e, DecalEdit{}, error));
    case ComponentKind::Reverb: return record_attachment<audio::ReverbZoneComponent>(*world(), m_stack, e, previous_steps, set_reverb_zone(e, audio::ReverbZoneComponent{}, error));
    case ComponentKind::Sprite: return record_attachment<runtime::SpriteComponent>(*world(), m_stack, e, previous_steps, set_sprite(e, runtime::SpriteComponent{}, error));
    case ComponentKind::Tilemap: return record_attachment<runtime::TilemapComponent>(*world(), m_stack, e, previous_steps, set_tilemap(e, runtime::TilemapComponent{}, error));
    case ComponentKind::Body2D: return record_attachment<runtime::RigidBody2DComponent>(*world(), m_stack, e, previous_steps, set_body_2d(e, runtime::RigidBody2DComponent{}, error));
    case ComponentKind::Collider2D: return record_attachment<runtime::Collider2DComponent>(*world(), m_stack, e, previous_steps, set_collider_2d(e, runtime::Collider2DComponent{}, error));
    case ComponentKind::Body: return record_attachment<physics::RigidBodyComponent>(*world(), m_stack, e, previous_steps, set_rigid_body(e, physics::RigidBodyComponent{}, error));
    case ComponentKind::Collider: return record_attachment<physics::ColliderComponent>(*world(), m_stack, e, previous_steps, set_collider(e, physics::ColliderComponent{}, error));
    case ComponentKind::Destructible: return record_attachment<runtime::DestructibleComponent>(*world(), m_stack, e, previous_steps, set_destructible(e, runtime::DestructibleComponent{}, error));
    case ComponentKind::Particles: return record_attachment<vfx::ParticleComponent>(*world(), m_stack, e, previous_steps, attach_particles(e, error));
    case ComponentKind::Cloth: return record_attachment<physics::ClothComponent>(*world(), m_stack, e, previous_steps, attach_cloth(e, error));
    case ComponentKind::Character: return record_attachment<physics::CharacterComponent>(*world(), m_stack, e, previous_steps, attach_character(e, error));
    case ComponentKind::Navmesh: return record_attachment<runtime::NavMeshComponent>(*world(), m_stack, e, previous_steps, attach_navmesh(e, error));
    case ComponentKind::NavAgent: return record_attachment<runtime::NavAgentComponent>(*world(), m_stack, e, previous_steps, attach_nav_agent(e, error));
    case ComponentKind::NavRoute: return record_attachment<runtime::NavRouteComponent>(*world(), m_stack, e, previous_steps, attach_nav_route(e, error));
    case ComponentKind::Terrain: return record_attachment<runtime::TerrainComponent>(*world(), m_stack, e, previous_steps, attach_terrain(e, error));
    case ComponentKind::Squad: return record_attachment<runtime::SquadComponent>(*world(), m_stack, e, previous_steps, attach_squad(e, error));
    case ComponentKind::Jammer: return record_attachment<runtime::JammerComponent>(*world(), m_stack, e, previous_steps, attach_jammer(e, error));
    case ComponentKind::Vehicle: return record_attachment<physics::VehicleComponent>(*world(), m_stack, e, previous_steps, attach_vehicle(e, error));
    case ComponentKind::Cable: return record_attachment<runtime::CableComponent>(*world(), m_stack, e, previous_steps, attach_cable(e, error));
    case ComponentKind::Weather: return record_attachment<runtime::WeatherComponent>(*world(), m_stack, e, previous_steps, attach_weather(e, error));
    case ComponentKind::RuleTile: return record_attachment<runtime::RuleTileComponent>(*world(), m_stack, e, previous_steps, attach_rule_tile(e, error));
    case ComponentKind::FlowField: return record_attachment<runtime::FlowFieldComponent>(*world(), m_stack, e, previous_steps, attach_flow_field(e, error));
    case ComponentKind::Talents: return record_attachment<runtime::TalentComponent>(*world(), m_stack, e, previous_steps, attach_talent_tree(e, error));
    case ComponentKind::Ragdoll: return record_attachment<runtime::RagdollComponent>(*world(), m_stack, e, previous_steps, attach_ragdoll(e, error));
    case ComponentKind::Inventory: return record_attachment<runtime::InventoryComponent>(*world(), m_stack, e, previous_steps, attach_inventory(e, error));
    case ComponentKind::SurfaceAudio: return record_attachment<runtime::SurfaceAudioComponent>(*world(), m_stack, e, previous_steps, attach_surface_audio(e, error));
    case ComponentKind::Weapon: return record_attachment<runtime::WeaponComponent>(*world(), m_stack, e, previous_steps, set_weapon(e, runtime::WeaponProfile{}, error));
    case ComponentKind::Health: return record_attachment<runtime::HealthComponent>(*world(), m_stack, e, previous_steps, set_health(e, runtime::HealthComponent{}, error));
    case ComponentKind::CombatAgent: return record_attachment<runtime::CombatAgentComponent>(*world(), m_stack, e, previous_steps, set_combat_agent(e, runtime::CombatAgentComponent{}, error));
    case ComponentKind::Faction: return record_attachment<runtime::FactionComponent>(*world(), m_stack, e, previous_steps, set_faction(e, runtime::FactionComponent{}, error));
    }
    error = "Unknown component";
    return false;
}
} // namespace nf::editor
