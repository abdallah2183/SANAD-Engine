#pragma once

#include <filesystem>
#include <string>

namespace nf::player {

struct EmbeddedGame {
    std::filesystem::path project_file;
    std::filesystem::path user_directory;
};

bool prepare_embedded_game(EmbeddedGame& game, std::string& error);
void show_sanad_splash();

} // namespace nf::player
