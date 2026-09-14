#pragma once

#include <string>

namespace nf::scene {

// Display name for an entity (Scene Outliner, Inspector title).
// Editor-owned in spirit but stored in the ECS world so it saves/loads with
// the scene. Empty means "use the generated label (Entity <id>)".
struct NameComponent {
    std::string name;
};

} // namespace nf::scene
