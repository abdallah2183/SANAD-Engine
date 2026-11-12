#pragma once
#include <NF/Editor/ComponentCatalog.hpp>
#include <optional>
#include <string>

namespace nf::editor {
struct ComponentPicker {
    std::string query;
    int group = -1;
    bool show_attached = false, focus_search = false;
    const ecs::World* world = nullptr;
    ecs::Entity entity = ecs::kInvalidEntity;
    uint64_t scene = 0;
    unsigned visible = 0;
    void begin(const ecs::World* source, ecs::Entity target, uint64_t serial) {
        query.clear(); group = -1; show_attached = false; focus_search = true;
        world = source; entity = target; scene = serial;
    }
};
std::optional<ComponentKind> component_picker_popup(ComponentPicker& picker, const ecs::World* world,
                                                     ecs::Entity entity, uint64_t scene);
} // namespace nf::editor
