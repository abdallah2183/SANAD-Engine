// RuntimeTests — the renderer exposure route the editor's Settings panel needs.
//
// `Runtime::renderer()` is const on purpose: a caller must not reach arbitrary
// renderer state from outside. That left the editor able to READ exposure and
// unable to write it, which is why Settings > Rendering shipped with shadows and
// no exposure, and why the `exposure` localization key has sat unused since
// Phase 15. `Runtime::set_exposure` is the narrow pair that closes it.
//
// What is worth pinning is that the value reaches the RENDERER, not just the
// runtime's own getter: a wrapper that stored the number and never forwarded it
// reads back correctly from `Runtime::exposure()` and changes nothing on screen.
// The refusal half lives in the same test because it needs the same device — the
// renderer's own setter stores whatever it is handed, so the guard has to be in
// the wrapper or nowhere.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <filesystem>
#include <limits>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;

NF_TEST(runtime_exposure_setter_reaches_the_renderer) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_exposure";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    auto device = rhi::create_device();
    if (!device) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("headless Vulkan device init failed");
    }

    {
        AssetRegistry reg;
        AssetManager manager(vfs, reg);
        // The Runtime owns the renderer, which owns RHI objects, so it must die
        // before the device is shut down — otherwise the device reports its live
        // resources as leaks.
        {
            Runtime rt(vfs, reg, manager, *device, nullptr);
            const rendering::Renderer3D* renderer = rt.renderer();
            NF_CHECK(renderer != nullptr);
            if (renderer != nullptr) {
                const float before = renderer->exposure();
                // The default is the renderer's own, untouched: the editor's
                // acceptance run pins its golden pixel count to this value.
                NF_CHECK_NEAR(before, 1.0f, 1e-6f);
                NF_CHECK_NEAR(rt.exposure(), before, 1e-6f);

                NF_CHECK(rt.set_exposure(2.5f));
                // Read back from the RENDERER, not from the runtime. This is the
                // assertion a store-and-forget wrapper fails.
                NF_CHECK_NEAR(renderer->exposure(), 2.5f, 1e-5f);
                NF_CHECK_NEAR(rt.exposure(), 2.5f, 1e-5f);

                // A value that would poison the tonemap is refused and the
                // previous exposure survives it. The renderer's own setter takes
                // whatever it is handed, so this guard is the wrapper's.
                NF_CHECK(!rt.set_exposure(std::numeric_limits<float>::quiet_NaN()));
                NF_CHECK(!rt.set_exposure(std::numeric_limits<float>::infinity()));
                NF_CHECK(!rt.set_exposure(0.0f));
                NF_CHECK(!rt.set_exposure(-1.0f));
                NF_CHECK_NEAR(renderer->exposure(), 2.5f, 1e-5f);
            }
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}
