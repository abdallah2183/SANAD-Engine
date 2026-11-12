#pragma once

#include <filesystem>
#include <string>

namespace nf::project {

struct AndroidPreviewResult {
    std::filesystem::path apk;
    std::filesystem::path log;
    std::wstring error;
};

/// Builds a debug-only Android preview without displaying a console window.
AndroidPreviewResult export_android_preview(const std::filesystem::path& project,
                                            const std::filesystem::path& engine_root);

} // namespace nf::project
