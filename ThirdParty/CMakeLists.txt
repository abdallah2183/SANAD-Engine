# ThirdParty CMakeLists.txt
# External dependencies for NOVAForge Engine.
# Each library is isolated and only linked where needed.

# Currently no third-party dependencies.
# As the engine grows, these will be added here:
# - Vulkan SDK (for RHI backend)
# - GLFW / SDL (optional windowing fallback)
# - stb_image, meshoptimizer, etc.

# --- Dear ImGui (editor UI, pinned snapshot, see imgui/VERSION_PIN.txt) -----
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
)
# NOTE: the Vulkan SDK include path for the Win32/Vulkan backends is added in
# Editor/CMakeLists.txt (NF_VULKAN_SDK_PATH is discovered by Engine/RHI, which
# is configured after ThirdParty).
# Third-party code stays warning-lenient on purpose: the engine's own /W4 /WX
# policy applies to first-party targets, not to vendored sources.
if(MSVC)
    target_compile_options(imgui PRIVATE /W1)
    set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(imgui PUBLIC NOMINMAX)
endif()
target_compile_features(imgui PUBLIC cxx_std_17)
