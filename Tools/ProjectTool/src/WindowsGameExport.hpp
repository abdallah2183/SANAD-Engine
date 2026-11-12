#pragma once

#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Project/ProjectPackager.hpp>

namespace nf::project {

bool write_windows_game(const BuildOptions& opts, const assets::ProjectDescriptor& project,
                        const std::filesystem::path& output_dir, BuildReport& report,
                        std::string& error);

} // namespace nf::project
