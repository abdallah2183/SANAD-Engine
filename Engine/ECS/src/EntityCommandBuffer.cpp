#include <NF/ECS/EntityCommandBuffer.hpp>

namespace nf::ecs {

Entity EntityCommandBuffer::create_entity() {
    Entity placeholder{ kPlaceholderFlag | m_placeholder_count++, 0 };
    return placeholder;
}

void EntityCommandBuffer::destroy_entity(Entity entity) {
    m_commands.push_back([entity](World& world, const std::unordered_map<Entity, Entity>& remap) {
        Entity target = resolve_entity(entity, remap);
        if (world.is_alive(target)) {
            world.destroy_entity(target);
        }
    });
}

void EntityCommandBuffer::add_action(std::function<void(World&)> action) {
    if (!action) return;
    m_commands.push_back([act = std::move(action)](World& world, const std::unordered_map<Entity, Entity>&) {
        act(world);
    });
}

void EntityCommandBuffer::playback(World& world) {
    // Phase 1: Instantiate real entities for all placeholders
    std::unordered_map<Entity, Entity> remap;
    remap.reserve(m_placeholder_count);

    for (u32 i = 0; i < m_placeholder_count; ++i) {
        Entity placeholder{ kPlaceholderFlag | i, 0 };
        Entity real = world.create_entity();
        remap[placeholder] = real;
    }

    // Phase 2: Execute all recorded commands in exact sequence
    for (auto& cmd : m_commands) {
        cmd(world, remap);
    }

    // Phase 3: Reset buffer
    clear();
}

void EntityCommandBuffer::clear() {
    m_commands.clear();
    m_placeholder_count = 0;
}

} // namespace nf::ecs
