#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/GameUiOverlay.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/TouchInput.hpp>
#include <NF/Scene/Scene.hpp>

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace nf;
namespace fs = std::filesystem;

bool extract_game(android_app* app, const fs::path& root) {
    AAssetManager* manager = app->activity->assetManager;
    AAsset* manifest = AAssetManager_open(manager, "game/manifest.txt", AASSET_MODE_BUFFER);
    if (!manifest) { NF_LOG_ERROR(LogCategory::Platform, "Android package has no game manifest"); return false; }
    const auto length = AAsset_getLength(manifest);
    if (length <= 0 || length > 2 * 1024 * 1024) { AAsset_close(manifest); return false; }
    std::string list(static_cast<size_t>(length), '\0');
    const int read = AAsset_read(manifest, list.data(), list.size());
    AAsset_close(manifest);
    if (read != length) return false;
    std::istringstream lines(list);
    std::string name;
    while (std::getline(lines, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        const fs::path relative(name);
        if (relative.is_absolute() || relative.has_root_path()) return false;
        for (const auto& part : relative) if (part == ".." || part == ".") return false;
        AAsset* asset = AAssetManager_open(manager, ("game/" + name).c_str(), AASSET_MODE_STREAMING);
        if (!asset) { NF_LOG_ERROR(LogCategory::Platform, "Missing packaged asset '{}'", name); return false; }
        const fs::path output = root / relative;
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (ec || !file) { AAsset_close(asset); return false; }
        std::array<char, 65536> buffer{};
        int n = 0;
        while ((n = AAsset_read(asset, buffer.data(), buffer.size())) > 0) file.write(buffer.data(), n);
        AAsset_close(asset);
        if (n < 0 || !file) return false;
    }
    return true;
}

struct Region { runtime::TouchControl control; const char* label; };

struct AndroidHost {
    android_app* app = nullptr;
    fs::path root;
    assets::VirtualFileSystem vfs;
    assets::AssetRegistry registry;
    std::unique_ptr<assets::AssetManager> assets;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<rhi::Swapchain> swapchain;
    std::unique_ptr<runtime::Runtime> game;
    std::unique_ptr<rendering::GameUiOverlay> overlay;
    runtime::TouchInputSource touch;
    std::vector<Region> regions;
    std::array<std::unique_ptr<rhi::Semaphore>, 2> available;
    std::array<std::unique_ptr<rhi::Fence>, 2> fences;
    std::array<std::unique_ptr<rhi::CommandBuffer>, 2> commands;
    std::vector<std::unique_ptr<rhi::Semaphore>> finished;
    ui::UiPointer pointer;
    int32_t pointer_id = -1;
    bool resumed = false, focused = false, ready = false;
    u32 frame = 0;
    std::chrono::steady_clock::time_point last_time;

    bool start() {
        stop();
        if (!app->window) return false;
        root = fs::path(app->activity->internalDataPath) / "sanad_game";
        if (!extract_game(app, root)) return false;
        std::string error;
        auto project = assets::ProjectDescriptor::load_from_file(root / "project.nfproj", error);
        if (!project) { NF_LOG_ERROR(LogCategory::Platform, "Project: {}", error); return false; }
        project->add_mount("saves://", fs::path(app->activity->internalDataPath) / "Saves");
        if (!project->apply_mounts(vfs, error)) { NF_LOG_ERROR(LogCategory::Platform, "Mount: {}", error); return false; }
        if (!registry.load(vfs, "content://AssetRegistry.nfreg", error))
            NF_LOG_WARN(LogCategory::Asset, "Registry: {}", error);
        device = rhi::create_device();
        rhi::DeviceDesc desc{};
        desc.window_handle = app->window;
        AAsset* validation = AAssetManager_open(app->activity->assetManager, "game/validation.enable", AASSET_MODE_BUFFER);
        desc.enable_validation = validation != nullptr;
        desc.disable_validation = !desc.enable_validation;
        if (validation) AAsset_close(validation);
        if (!device || !device->init(desc)) { NF_LOG_ERROR(LogCategory::RHI, "Android device initialization failed"); stop(); return false; }
        rhi::SwapchainDesc sc{};
        sc.width = static_cast<u32>(ANativeWindow_getWidth(app->window));
        sc.height = static_cast<u32>(ANativeWindow_getHeight(app->window));
        sc.image_count = 2;
        swapchain = device->create_swapchain(sc);
        if (!swapchain) { NF_LOG_ERROR(LogCategory::RHI, "Android swapchain initialization failed"); stop(); return false; }
        assets = std::make_unique<assets::AssetManager>(vfs, registry);
        game = std::make_unique<runtime::Runtime>(vfs, registry, *assets, *device, swapchain.get());
        game->set_mobile_render_defaults();
        game->set_playing(true);
        game->set_input_source(&touch);
        game->set_viewport(static_cast<float>(swapchain->width()), static_cast<float>(swapchain->height()));
        if (!game->load_scene(project->startup_scene(), error)) {
            NF_LOG_ERROR(LogCategory::Core, "Scene: {}", error); stop(); return false;
        }
        regions.clear();
        for (const auto e : game->scene()->world().query<gameplay::GameplayModuleComponent>()) {
            const auto* module = game->scene()->world().get<gameplay::GameplayModuleComponent>(e);
            if (module && module->enabled && module->module_name == "PlatformerController") {
                regions = {{{"move_x", 0.03f, 0.73f, 0.15f, 0.20f, -1.0f}, "<"},
                           {{"move_x", 0.20f, 0.73f, 0.15f, 0.20f, 1.0f}, ">"},
                           {{"jump", 0.79f, 0.73f, 0.18f, 0.20f, 1.0f}, "JUMP"}};
                break;
            }
        }
        touch.clear_controls();
        for (const auto& region : regions) touch.add_control(region.control);
        overlay = std::make_unique<rendering::GameUiOverlay>();
        if (!overlay->init(*device, nullptr, game->shader_directory() / "UI") || !overlay->attach_swapchain(*swapchain)) {
            NF_LOG_ERROR(LogCategory::Core, "Android UI initialization failed"); stop(); return false;
        }
        game->game_ui().set_text_measurer(overlay->make_measurer());
        for (size_t i = 0; i < 2; ++i) {
            available[i] = device->create_semaphore(); fences[i] = device->create_fence(true);
            commands[i] = device->create_command_buffer();
            if (!available[i] || !fences[i] || !commands[i]) { stop(); return false; }
        }
        for (u32 i = 0; i < swapchain->image_count(); ++i) {
            auto semaphore = device->create_semaphore();
            if (!semaphore) { stop(); return false; }
            finished.push_back(std::move(semaphore));
        }
        frame = 0;
        last_time = std::chrono::steady_clock::now();
        ready = true;
        NF_LOG_INFO(LogCategory::Platform, "Android game ready: {}x{}", swapchain->width(), swapchain->height());
        return true;
    }

    void stop() {
        ready = false;
        touch.cancel_all(); pointer = {}; pointer_id = -1;
        if (device) device->wait_idle();
        if (game) { game->set_input_source(nullptr); game->shutdown(); }
        if (overlay) overlay->shutdown();
        overlay.reset(); game.reset(); assets.reset();
        for (auto& item : commands) item.reset();
        for (auto& item : fences) item.reset();
        for (auto& item : available) item.reset();
        finished.clear(); swapchain.reset();
        if (device) { device->shutdown(); device.reset(); }
    }

    void pause_audio() {
        if (!game) return;
        auto* output = game->audio_device();
        if (output && output->accepts_push() && output->is_initialized()) output->shutdown();
    }

    void resume_audio() {
        if (!game) return;
        auto* output = game->audio_device();
        if (output && output->accepts_push() && !output->is_initialized()) {
            if (!output->initialize(audio::kDefaultSampleRate, audio::kDefaultBufferFrames)) {
                NF_LOG_WARN(LogCategory::Audio, "Android audio could not resume");
            }
        }
    }

    bool over_controls(float x, float y) const {
        for (const auto& region : regions) {
            const auto& c = region.control;
            if (x >= c.x && x < c.x+c.width && y >= c.y && y < c.y+c.height) return true;
        }
        return false;
    }

    void draw_frame() {
        if (!ready || !resumed || !focused || !app->window) return;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_time).count(), 0.0f, 0.1f);
        last_time = now;
        game->set_ui_pointer(pointer);
        game->update(dt);
        const u32 slot = frame % 2;
        fences[slot]->wait();
        const u32 image = swapchain->acquire_next_image(*available[slot]);
        if (image == 0xFFFFFFFFu || image >= finished.size()) { stop(); return; }
        commands[slot]->reset(); commands[slot]->begin();
        game->render(image, *commands[slot], slot);
        const float width = static_cast<float>(swapchain->width());
        const float height = static_cast<float>(swapchain->height());
        auto display = game->game_ui().snapshot(width, height);
        for (const auto& region : regions) {
            const auto& c = region.control;
            display.rects.push_back({{c.x*width, c.y*height, c.width*width, c.height*height}, {0.12f, 0.20f, 0.30f, 0.68f}});
            ui::DlText text;
            text.x = (c.x + c.width*0.5f)*width; text.y = (c.y + c.height*0.35f)*height;
            text.text = region.label; text.size = 22.0f; text.align = ui::TextAlign::Center;
            display.texts.push_back(std::move(text));
        }
        overlay->begin_frame(dt); overlay->draw(display, width, height); overlay->end_frame();
        if (!overlay->render(*commands[slot], *swapchain->get_texture(image), image, slot, swapchain->width(), swapchain->height())) {
            commands[slot]->end(); stop(); return;
        }
        commands[slot]->end();
        const std::array<const rhi::Semaphore*, 1> waits{available[slot].get()};
        const std::array<rhi::PipelineStage, 1> stages{rhi::PipelineStage::ColorAttachmentOutput};
        const std::array<const rhi::Semaphore*, 1> signals{finished[image].get()};
        rhi::SubmitInfo submit{};
        submit.wait_semaphores = waits; submit.wait_stages = stages; submit.signal_semaphores = signals;
        submit.signal_fence = fences[slot].get();
        fences[slot]->reset(); device->submit(*commands[slot], submit); swapchain->present(image, signals);
        ++frame;
    }
};

void command(android_app* app, int32_t cmd) {
    auto& host = *static_cast<AndroidHost*>(app->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: host.start(); break;
    case APP_CMD_TERM_WINDOW: host.stop(); break;
    case APP_CMD_RESUME: host.resumed = true; host.resume_audio(); host.last_time = std::chrono::steady_clock::now(); break;
    case APP_CMD_PAUSE: host.resumed = false; host.pause_audio(); host.touch.cancel_all(); host.pointer = {}; host.pointer_id = -1; break;
    case APP_CMD_GAINED_FOCUS: host.focused = true; host.last_time = std::chrono::steady_clock::now(); break;
    case APP_CMD_LOST_FOCUS: host.focused = false; host.touch.cancel_all(); host.pointer = {}; host.pointer_id = -1; break;
    default: break;
    }
}

int32_t input(android_app* app, AInputEvent* event) {
    auto& host = *static_cast<AndroidHost*>(app->userData);
    if (!host.ready || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION || !app->window) return 0;
    const float width = static_cast<float>(ANativeWindow_getWidth(app->window));
    const float height = static_cast<float>(ANativeWindow_getHeight(app->window));
    if (width <= 0.0f || height <= 0.0f) return 0;
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (kind == AMOTION_EVENT_ACTION_CANCEL) { host.touch.cancel_all(); host.pointer = {}; host.pointer_id = -1; return 1; }
    const size_t count = AMotionEvent_getPointerCount(event);
    if (index >= count) return 0;
    auto update = [&](size_t i, bool down) {
        const int32_t id = AMotionEvent_getPointerId(event, i);
        const float x = AMotionEvent_getX(event, i) / width;
        const float y = AMotionEvent_getY(event, i) / height;
        if (down) host.touch.touch_down(id, x, y); else host.touch.touch_move(id, x, y);
        if (host.pointer_id == id) { host.pointer.x = x; host.pointer.y = y; }
    };
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        update(index, true);
        const float x = AMotionEvent_getX(event, index) / width, y = AMotionEvent_getY(event, index) / height;
        if (host.pointer_id < 0 && !host.over_controls(x, y)) {
            host.pointer_id = AMotionEvent_getPointerId(event, index);
            host.pointer = {x, y, true, true};
        }
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t i = 0; i < count; ++i) update(i, false);
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) {
        const int32_t id = AMotionEvent_getPointerId(event, index);
        host.touch.touch_up(id);
        if (host.pointer_id == id) { host.pointer.down = false; host.pointer_id = -1; }
    }
    return 1;
}
}

void android_main(android_app* app) {
    nf::Logger::instance().add_sink([](const nf::LogMessage& message) {
        const int priority = message.level >= nf::LogLevel::Error ? ANDROID_LOG_ERROR :
                             message.level >= nf::LogLevel::Warn ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
        __android_log_print(priority, "SANAD", "%s", message.text.c_str());
    });
    nf::Logger::instance().set_min_level(nf::LogLevel::Info);
    nf::JobSystem::instance().init(3);
    AndroidHost host;
    host.app = app; app->userData = &host; app->onAppCmd = command; app->onInputEvent = input;
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const bool playing = host.ready && host.resumed && host.focused;
        int ident = ALooper_pollOnce(playing ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (ident >= 0 && source) source->process(app, source);
        if (app->destroyRequested) break;
        host.draw_frame();
    }
    host.stop();
    nf::JobSystem::instance().shutdown();
}
