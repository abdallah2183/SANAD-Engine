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

# --- miniaudio (header-only audio decoding: WAV/OGG-Vorbis/MP3/FLAC) ---------
# Used by NFAudio for the import pipeline only (decode file -> AudioBuffer);
# output stays on the engine's AudioDevice seam (Null/WASAPI). Single header,
# implementation compiled once inside NFAudio (see Engine/Audio).
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio
)

# --- cgltf (header-only glTF 2.0 parser) -------------------------------------
# Used by NFAssets for the glTF import pipeline (mesh/nodes/materials).
# Single header, implementation compiled once inside NFAssets.
add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/cgltf
)

# --- Lua 5.4 (gameplay scripting) --------------------------------------------
# Used by NFScripting for the Lua VM. Vendored C sources, built as a static
# library without the standalone interpreter/repl (lua.c/luac.c).
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/*.c
)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/src
)
# Third-party code stays warning-lenient on purpose.
if(MSVC)
    target_compile_options(lua PRIVATE /W1)
    set_property(TARGET lua PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(lua PUBLIC NOMINMAX)
endif()
