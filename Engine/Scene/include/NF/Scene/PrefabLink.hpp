#pragma once

// NF/Scene/PrefabLink.hpp — link from an instance root to its template file.
//
// A prefab IS a .nfscene file (same serializer). An instantiated root carries
// this link; its whole subtree is the instance. Local edits are free
// (overrides); Apply pushes the subtree back to the file, Revert replaces the
// subtree from the file. Nested links are preserved verbatim and only the top
// link drives Apply/Revert.

#include <string>

namespace nf::scene {

struct PrefabLinkComponent {
    std::string prefab_path; // content:// logical path of the template file
};

} // namespace nf::scene
