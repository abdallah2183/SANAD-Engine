# Editor — NOVAForge native editor (Phase 4: Foundation, Phase 5: Workflows)
#
# NFEditorCore is window/RHI-free editor logic (selection, outliner, commands,
# asset browser, inspector edits, gizmo math, play isolation, viewport target
# lifecycle, console). It is fully covered by EditorTests without a window.
#
# NOVAForgeEditor is the native shell: Win32 window + Vulkan device + Runtime
# viewport + Dear ImGui (docking) panels.

# The Vulkan SDK include path (discovered by Engine/RHI) is needed to compile
# ImGui's Win32/Vulkan backends and any editor translation unit that pulls in
# the backend headers.
if(DEFINED NF_VULKAN_SDK_PATH)
    target_include_directories(imgui PRIVATE "${NF_VULKAN_SDK_PATH}/Include")
endif()

set(NFEDITOR_CORE_SOURCES
    src/Selection.cpp
    src/Outliner.cpp
    src/Commands.cpp
    src/AssetBrowser.cpp
    src/Inspector.cpp
    src/Gizmo.cpp
    src/FileWatcher.cpp
    src/HotReload.cpp
    src/ImportQueue.cpp
    src/PlayMode.cpp
    src/Prefabs.cpp
    src/Viewport.cpp
    src/Console.cpp
    src/ProfilerSession.cpp
    src/ReflectedInspector.cpp
    src/OrbitCameraModule.cpp
    src/EditorApp.cpp
    src/PreviewPixels.cpp
    src/AudioPreview.cpp
    src/ui/UiRenderer.cpp
)

add_library(NFEditorCore STATIC
    ${NFEDITOR_CORE_SOURCES}
)
target_include_directories(NFEditorCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(NFEditorCore PUBLIC NFCore NFEcs NFScene NFAssets NFRuntime NFJobs NFPhysics NFGameplay NFUI imgui)
nf_target_set_warnings(NFEditorCore)

add_executable(NOVAForgeEditor
    src/main.cpp
    src/ui/Panels.cpp
    src/ui/Toolbar.cpp
    src/ui/UiBackend.cpp
    # Pre-editor project launcher (UX2 E2). App-target only: it is a Win32
    # window shown before the VFS mounts exist, and its pure list/validation
    # rules are header-inline so EditorTests reaches them without linking this.
    src/ProjectLauncher.cpp
    # GPU thumbnail upload: the shell owns the device, so the cache lives here
    # and NFEditorCore stays device-free (panels reach it through the
    # EditorApp preview_texture hook).
    src/TexturePreviewCache.cpp
)
target_include_directories(NOVAForgeEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(NOVAForgeEditor PRIVATE
    NFEditorCore NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets NFRuntime
    NFProjectTool NFUI
    imgui
    # SetWindowSubclass (button hover) in ProjectLauncher.cpp.
    comctl32
    # GDI+ PNG thumbnails in the project launcher shell (ProjectLauncher.cpp).
    gdiplus
)
target_compile_definitions(NOVAForgeEditor PRIVATE
    "NF_EDITOR_IMGUI_SHADER_DIR=\"${NF_EDITOR_IMGUI_SHADER_DIR}\""
    # The Build action packages a project, which needs to know where the project
    # template and the compiled renderer shaders live — the same definitions the
    # `nf` CLI receives.
    "NF_TEMPLATE_DIR=\"${CMAKE_SOURCE_DIR}/Templates/Default\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)
nf_target_set_warnings(NOVAForgeEditor)

if(WIN32)
    # Application manifest (comctl32 v6 + system DPI awareness) for the editor
    # and its pre-editor launcher, merged into the exe's existing id-1 manifest
    # after each link. Two earlier approaches were tried and rejected on
    # evidence: an .rc resource collides with CMake's own generated
    # manifest.res (CVT1100 "duplicate resource"), and /MANIFESTINPUT demands
    # /MANIFEST:EMBED, which is not what CMake's vs_link_exe passes (LNK1220).
    # mt.exe is the tool that is designed for exactly this merge; it is on PATH
    # under vcvars64 (build_nf.bat) and under the nfb.sh env alike.
    # comctl v6 is what makes the launcher's EM_SETCUEBANNER search hint appear
    # at all; DPI awareness stops Windows bitmap-stretching the window above
    # 100% scale.
    add_custom_command(TARGET NOVAForgeEditor POST_BUILD
        COMMAND mt.exe -nologo
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/NOVAForgeEditor.manifest"
                "-outputresource:$<TARGET_FILE:NOVAForgeEditor>;1"
        COMMENT "Merging NOVAForgeEditor.manifest (comctl32 v6 + DPI awareness)"
        VERBATIM
    )
endif()

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NOVAForgeEditor NFEditorImGuiShaders)
endif()

if(WIN32 AND DEFINED NF_VULKAN_SDK_PATH)
    # ImGui's Vulkan backend headers need the SDK includes wherever they are
    # pulled in, and the backend translation unit references Vk API functions
    # directly (the engine itself keeps runtime-loaded entry points and links
    # no import lib).
    target_include_directories(NOVAForgeEditor PRIVATE "${NF_VULKAN_SDK_PATH}/Include")
    target_link_libraries(NOVAForgeEditor PRIVATE "${NF_VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
endif()

# MSVC: match the static runtime used by engine libraries
if(MSVC)
    set_property(TARGET NFEditorCore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NOVAForgeEditor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
