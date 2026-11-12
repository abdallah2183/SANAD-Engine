#include <NF/Editor/Inspector2D.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Commands.hpp>
#include <cmath>

namespace nf::editor {
namespace {
template<typename T> class Set2DCommand final : public ICommand {
public:
    Set2DCommand(const ecs::World& world, ecs::Entity entity, const T& value, const char* label)
        : entity(entity), after(value), name(label) {
        if (const auto* old = world.get<T>(entity)) { before = *old; had_before = true; }
    }
    void apply(ecs::World& world) override {
        if (world.is_alive(entity)) world.add<T>(entity, after);
    }
    void undo(ecs::World& world) override {
        if (!world.is_alive(entity)) return;
        if (had_before) world.add<T>(entity, before);
        else world.remove<T>(entity);
    }
    std::string label() const override { return name; }
    ecs::Entity target() const override { return entity; }
    void remap_entity(ecs::Entity old_entity, ecs::Entity new_entity) override {
        if (entity == old_entity) entity = new_entity;
    }
private:
    ecs::Entity entity;
    T before{}, after{};
    bool had_before = false;
    std::string name;
};
bool range(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
bool valid(const runtime::SpriteComponent& value) {
    return range(value.width, .01f, 1000) && range(value.height, .01f, 1000) &&
           range(value.color_r, 0, 1) && range(value.color_g, 0, 1) &&
           range(value.color_b, 0, 1) && range(value.color_a, 0, 1) &&
           std::isfinite(value.u0) && std::isfinite(value.u1) &&
           std::isfinite(value.v0) && std::isfinite(value.v1);
}
bool valid(const runtime::TilemapComponent& value) {
    return range(value.tile_size, 1, 512) && value.columns >= 1 && value.columns <= 1024 &&
           value.rows >= 1 && value.rows <= 1024;
}
bool valid(const runtime::RigidBody2DComponent& value) {
    return value.body_type >= 0 && value.body_type <= 2 &&
           range(value.mass, value.body_type == 0 ? .01f : 0, 10000) &&
           range(value.linear_damping, 0, 100) && range(value.angular_damping, 0, 100);
}
bool valid(const runtime::Collider2DComponent& value) {
    return value.shape >= 0 && value.shape <= 1 && range(value.size_x, .01f, 1000) &&
           range(value.size_y, .01f, 1000) && range(value.radius, .01f, 500) &&
           range(value.friction, 0, 10) && range(value.restitution, 0, 1);
}
template<typename T> std::unique_ptr<ICommand> make_2d_command(ecs::World& world, ecs::Entity entity,
                                                             const T& edit, std::string& error,
                                                             const char* label) {
    if (!world.is_alive(entity)) { error = "Entity is not alive"; return nullptr; }
    if (!valid(edit)) { error = "2D component values are outside the supported range"; return nullptr; }
    return std::make_unique<Set2DCommand<T>>(world, entity, edit, label);
}
} // namespace

std::unique_ptr<ICommand> make_sprite_command(ecs::World& world, ecs::Entity entity,
                                            const runtime::SpriteComponent& edit, std::string& error) {
    return make_2d_command(world, entity, edit, error, "Edit sprite");
}
bool EditorApp::set_sprite(ecs::Entity entity, const runtime::SpriteComponent& edit, std::string& error) {
    if (!require_editable(error)) return false;
    if (!world()) { error = "No scene open"; return false; }
    auto command = make_sprite_command(*world(), entity, edit, error);
    if (!command) return false;
    m_stack.push(std::move(command), *world());
    after_mutation(entity);
    return true;
}

std::unique_ptr<ICommand> make_tilemap_command(ecs::World& world, ecs::Entity entity,
                                            const runtime::TilemapComponent& edit, std::string& error) {
    return make_2d_command(world, entity, edit, error, "Edit tilemap");
}
bool EditorApp::set_tilemap(ecs::Entity entity, const runtime::TilemapComponent& edit, std::string& error) {
    if (!require_editable(error)) return false;
    if (!world()) { error = "No scene open"; return false; }
    auto command = make_tilemap_command(*world(), entity, edit, error);
    if (!command) return false;
    m_stack.push(std::move(command), *world());
    after_mutation(entity);
    return true;
}

std::unique_ptr<ICommand> make_body_2d_command(ecs::World& world, ecs::Entity entity,
                                            const runtime::RigidBody2DComponent& edit, std::string& error) {
    return make_2d_command(world, entity, edit, error, "Edit body_2d");
}
bool EditorApp::set_body_2d(ecs::Entity entity, const runtime::RigidBody2DComponent& edit, std::string& error) {
    if (!require_editable(error)) return false;
    if (!world()) { error = "No scene open"; return false; }
    auto command = make_body_2d_command(*world(), entity, edit, error);
    if (!command) return false;
    m_stack.push(std::move(command), *world());
    after_mutation(entity);
    return true;
}

std::unique_ptr<ICommand> make_collider_2d_command(ecs::World& world, ecs::Entity entity,
                                            const runtime::Collider2DComponent& edit, std::string& error) {
    return make_2d_command(world, entity, edit, error, "Edit collider_2d");
}
bool EditorApp::set_collider_2d(ecs::Entity entity, const runtime::Collider2DComponent& edit, std::string& error) {
    if (!require_editable(error)) return false;
    if (!world()) { error = "No scene open"; return false; }
    auto command = make_collider_2d_command(*world(), entity, edit, error);
    if (!command) return false;
    m_stack.push(std::move(command), *world());
    after_mutation(entity);
    return true;
}
} // namespace nf::editor
