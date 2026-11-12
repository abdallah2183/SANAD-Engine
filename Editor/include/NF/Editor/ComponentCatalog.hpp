#pragma once
#include <NF/ECS/ECS.hpp>
#include <array>
#include <string_view>

namespace nf::editor {
enum class ComponentGroup { Rendering, TwoD, Physics, World, Gameplay };
enum class ComponentKind { Camera, Sun, Sky, PostProcess, Decal, Reverb, Sprite, Tilemap, Body2D, Collider2D, Body, Collider, Destructible, Particles, Cloth, Character, Navmesh, NavAgent, NavRoute, Terrain, Squad, Jammer, Vehicle, Cable, Weather, RuleTile, FlowField, Talents, Ragdoll, Inventory, SurfaceAudio, Weapon, Health, CombatAgent, Faction };
struct ComponentEntry {
    ComponentKind kind;
    ComponentGroup group;
    const char* label_key;
    const char* keywords;
};
inline constexpr std::array<ComponentEntry, 35> kComponents{{
    {ComponentKind::Camera, ComponentGroup::Rendering, "camera", "camera view"},
    {ComponentKind::Sun, ComponentGroup::Rendering, "directional_light", "sun directional light"},
    {ComponentKind::Sky, ComponentGroup::World, "sky", "sky environment"},
    {ComponentKind::PostProcess, ComponentGroup::Rendering, "post_process", "post processing effects"},
    {ComponentKind::Decal, ComponentGroup::Rendering, "decal", "decal projection"},
    {ComponentKind::Reverb, ComponentGroup::World, "reverb_zone", "audio sound reverb"},
    {ComponentKind::Sprite, ComponentGroup::TwoD, "sprite_2d", "sprite image 2d"},
    {ComponentKind::Tilemap, ComponentGroup::TwoD, "tilemap_2d", "tilemap tiles 2d"},
    {ComponentKind::Body2D, ComponentGroup::TwoD, "rigidbody_2d", "physics rigid body 2d"},
    {ComponentKind::Collider2D, ComponentGroup::TwoD, "collider_2d", "physics collider collision 2d"},
    {ComponentKind::Body, ComponentGroup::Physics, "rigid_body", "physics rigid body 3d"},
    {ComponentKind::Collider, ComponentGroup::Physics, "collider", "physics collision 3d"},
    {ComponentKind::Destructible, ComponentGroup::Physics, "destructible", "destruction break physics"},
    {ComponentKind::Particles, ComponentGroup::Rendering, "particles", "particles effects vfx"},
    {ComponentKind::Cloth, ComponentGroup::Physics, "cloth", "cloth fabric physics"},
    {ComponentKind::Character, ComponentGroup::Physics, "character", "character controller player physics"},
    {ComponentKind::Navmesh, ComponentGroup::World, "navmesh", "navigation navmesh ai"},
    {ComponentKind::NavAgent, ComponentGroup::World, "nav_agent", "navigation agent ai"},
    {ComponentKind::NavRoute, ComponentGroup::World, "nav_route", "navigation route patrol ai"},
    {ComponentKind::Terrain, ComponentGroup::World, "terrain", "terrain ground landscape"},
    {ComponentKind::Squad, ComponentGroup::Gameplay, "squad", "squad team ai"},
    {ComponentKind::Jammer, ComponentGroup::Gameplay, "jammer", "jammer ai"},
    {ComponentKind::Vehicle, ComponentGroup::Physics, "vehicle", "vehicle car physics"},
    {ComponentKind::Cable, ComponentGroup::Physics, "cable_physics", "cable rope physics"},
    {ComponentKind::Weather, ComponentGroup::World, "weather_system", "weather rain wind"},
    {ComponentKind::RuleTile, ComponentGroup::TwoD, "rule_tile", "rule tile 2d"},
    {ComponentKind::FlowField, ComponentGroup::World, "flow_field", "flow field navigation"},
    {ComponentKind::Talents, ComponentGroup::Gameplay, "talent_tree", "talents progression skill"},
    {ComponentKind::Ragdoll, ComponentGroup::Physics, "ragdoll_blend", "ragdoll physics"},
    {ComponentKind::Inventory, ComponentGroup::Gameplay, "grid_inventory", "inventory grid items"},
    {ComponentKind::SurfaceAudio, ComponentGroup::World, "surface_audio", "surface audio sound footsteps"},
    {ComponentKind::Weapon, ComponentGroup::Gameplay, "weapon", "weapon gun combat"},
    {ComponentKind::Health, ComponentGroup::Gameplay, "health", "health hit points combat"},
    {ComponentKind::CombatAgent, ComponentGroup::Gameplay, "combat_agent", "combat agent ai"},
    {ComponentKind::Faction, ComponentGroup::Gameplay, "faction", "faction team combat"},
}};
bool has_component(const ecs::World& world, ecs::Entity entity, ComponentKind kind);
bool component_entry_matches(const ComponentEntry& entry, std::string_view query);
} // namespace nf::editor
