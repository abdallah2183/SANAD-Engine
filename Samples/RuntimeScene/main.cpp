// Samples/RuntimeScene/main.cpp — RuntimeScene sample: loads a .nfscene via VFS/Registry/Manager and renders it

#include <NF/Runtime/Application.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    nf::runtime::ApplicationConfig config;
    config.title = "NOVAForge — RuntimeScene";
    config.width = 1280;
    config.height = 720;
    config.vsync = true;
    config.scene_path = "content://Scenes/Example.nfscene";
    config.max_frames = 0;
    config.validation = false;
    config.headless = false;

    for (int i=1;i<argc;++i){
        std::string_view arg = argv[i];
        if (arg=="--scene" && i+1<argc) config.scene_path = argv[++i];
        else if (arg.rfind("--scene=",0)==0) config.scene_path = std::string(arg.substr(8));
        else if (arg=="--frames" && i+1<argc) config.max_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg.rfind("--frames=",0)==0) config.max_frames = static_cast<uint32_t>(std::atoi(arg.substr(9).data()));
        else if (arg=="--validation") config.validation = true;
        else if (arg=="--headless") config.headless = true;
        else if (arg=="--help" || arg=="-h") {
            std::cout << "NOVAForge RuntimeScene\n"
                      << "  --scene <logical>   Scene to load (default content://Scenes/Example.nfscene)\n"
                      << "  --frames N          Run N frames then exit (0 = until close)\n"
                      << "  --validation        Enable Vulkan validation\n"
                      << "  --headless          Run headless (no window, for tests)\n";
            return 0;
        }
    }

    // Allow env vars for CI (use _dupenv_s on MSVC)
#ifdef _MSC_VER
    char* buf=nullptr; size_t sz=0;
    if (_dupenv_s(&buf,&sz,"NF_RUNTIME_SCENE")==0 && buf){ config.scene_path=buf; std::free(buf); }
    char* buf2=nullptr; size_t sz2=0;
    if (_dupenv_s(&buf2,&sz2,"NF_RUNTIME_FRAMES")==0 && buf2){ config.max_frames=static_cast<uint32_t>(std::atoi(buf2)); std::free(buf2); }
    char* buf3=nullptr; size_t sz3=0;
    if (_dupenv_s(&buf3,&sz3,"NF_RUNTIME_VALIDATION")==0 && buf3){ config.validation=true; std::free(buf3); }
    char* buf4=nullptr; size_t sz4=0;
    if (_dupenv_s(&buf4,&sz4,"NF_RUNTIME_HEADLESS")==0 && buf4){ config.headless=true; std::free(buf4); }
#else
    if (auto e = std::getenv("NF_RUNTIME_SCENE")) config.scene_path = e;
    if (auto e = std::getenv("NF_RUNTIME_FRAMES")) config.max_frames = static_cast<uint32_t>(std::atoi(e));
    if (std::getenv("NF_RUNTIME_VALIDATION")) config.validation = true;
    if (std::getenv("NF_RUNTIME_HEADLESS")) config.headless = true;
#endif

    nf::runtime::Application app(config);
    return app.run();
}
