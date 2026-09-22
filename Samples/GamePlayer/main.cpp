// Samples/GamePlayer/main.cpp — NFGamePlayer: the standalone game window.
//
// This is what the editor's Play button launches: it opens a real window,
// loads a .nfscene through the same VFS mounts the editor used, and runs the
// full Runtime loop — physics, animation, audio, gameplay (PlayerController /
// OrbitCamera read the real keyboard) — until the window is closed.
//
// The editor writes the current scene to a mounted scheme before spawning
// this player, so "Play" always plays exactly what is on screen, saved or not.

#include <NF/Runtime/Application.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    nf::runtime::ApplicationConfig config;
    config.title = "NOVAForge Game";
    config.width = 1280;
    config.height = 720;
    config.vsync = true;
    config.max_frames = 0;
    config.validation = false;
    config.headless = false;
    // Empty: the editor always passes --scene. Without one, the project's
    // startup scene (or content://Scenes/Example.nfscene) is used.
    config.scene_path = "";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--scene" && i + 1 < argc) {
            config.scene_path = argv[++i];
        } else if (arg.rfind("--scene=", 0) == 0) {
            config.scene_path = std::string(arg.substr(8));
        } else if (arg == "--project" && i + 1 < argc) {
            config.project_path = argv[++i];
        } else if (arg.rfind("--project=", 0) == 0) {
            config.project_path = std::string(arg.substr(10));
        } else if (arg == "--frames" && i + 1 < argc) {
            config.max_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg.rfind("--frames=", 0) == 0) {
            config.max_frames = static_cast<uint32_t>(std::atoi(arg.substr(9).data()));
        } else if (arg == "--validation") {
            config.validation = true;
        } else if (arg == "--headless") {
            config.headless = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "NOVAForge GamePlayer\n"
                      << "  --scene <logical>     Scene to play (e.g. cache://EditorPlaySession.nfscene)\n"
                      << "  --project <file>      .nfproj providing the VFS mounts and window settings\n"
                      << "  --frames N            Run N frames then exit (0 = until closed)\n"
                      << "  --validation          Enable Vulkan validation\n"
                      << "  --headless            Run headless (no window, for tests)\n"
                      << "\nControls: arrows/WASD move, Space jump, Shift sprint, E interact.\n";
            return 0;
        }
    }

    nf::runtime::Application app(config);
    return app.run();
}
