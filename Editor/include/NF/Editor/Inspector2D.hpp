#pragma once
#include <NF/Editor/Inspector.hpp>

namespace nf::editor {
std::unique_ptr<ICommand> make_sprite_command(ecs::World&, ecs::Entity, const runtime::SpriteComponent&, std::string&);
std::unique_ptr<ICommand> make_tilemap_command(ecs::World&, ecs::Entity, const runtime::TilemapComponent&, std::string&);
std::unique_ptr<ICommand> make_body_2d_command(ecs::World&, ecs::Entity, const runtime::RigidBody2DComponent&, std::string&);
std::unique_ptr<ICommand> make_collider_2d_command(ecs::World&, ecs::Entity, const runtime::Collider2DComponent&, std::string&);
} // namespace nf::editor
