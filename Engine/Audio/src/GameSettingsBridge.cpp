#include <NF/Audio/GameSettingsBridge.hpp>

#include <NF/Core/Logger.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace nf::audio {

bool load_game_audio_settings(const std::string& path,
                              GameAudioSettings& out,
                              bool& out_found) {
    // Reset first: a caller that reuses one object across a reload must never
    // see last session's volumes leak into this one.
    out.reset_to_mixer_defaults();
    out_found = false;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        // First run, or a path that does not exist yet. Not a failure: the
        // shipped defaults are exactly right, and reporting an error here would
        // train callers to ignore the return value.
        return true;
    }
    out_found = true;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        NF_LOG_WARN(LogCategory::Core,
                    "Audio settings: '{}' exists but could not be opened; "
                    "using defaults", path);
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();

    const u32 applied = out.apply_settings_text(text.str());
    if (applied == 0) {
        // The file existed but held none of our keys. Worth a warning: the player
        // will not hear their saved volumes and the file is right there to look
        // at. Not an error — a settings file shared with other systems is normal.
        NF_LOG_WARN(LogCategory::Core,
                    "Audio settings: '{}' contained no audio.volume.* entries; "
                    "using defaults", path);
    }
    return true;
}

bool save_game_audio_settings(const std::string& path, const GameAudioSettings& in) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        // Best effort: a settings file whose directory does not exist yet is
        // the normal first-run case, and refusing to save for that would make
        // the first write the one that fails.
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        NF_LOG_ERROR(LogCategory::Core,
                     "Audio settings: could not open '{}' for writing; the "
                     "player's volume choices were not saved", path);
        return false;
    }
    std::string text;
    in.append_settings_text(text);
    file << text;
    // Closed explicitly so a full disk or a write error surfaces here rather
    // than in some later destructor, where the caller has already been told the
    // save succeeded.
    file.close();
    if (!file) {
        NF_LOG_ERROR(LogCategory::Core,
                     "Audio settings: writing '{}' failed; the player's volume "
                     "choices were not saved", path);
        return false;
    }
    return true;
}

} // namespace nf::audio
