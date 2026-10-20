# NFPlayer — the standalone game runtime.
#
# Deliberately thin: it parses arguments, reads a project descriptor, and hands
# off to nf::runtime::Application. Everything that makes a game run already lives
# in NFRuntime; the only thing this executable adds is "start from a project
# instead of from the engine source tree".

add_executable(NFPlayer
    main.cpp
)

target_link_libraries(NFPlayer PRIVATE NFCore NFAssets NFRuntime)
nf_target_set_warnings(NFPlayer)

# NFRuntime bakes NF_BASIC3D_SHADER_DIR in and loads the Basic3D pipelines
# from it at startup, so a player built without the compiled SPIR-V present
# starts up with no shaders and renders nothing. The same edge as the editor
# and the samples declare (Editor/CMakeLists.txt, Samples/CMakeLists.txt).
#
# The targets only exist when glslc was found at configure time, hence the
# TARGET guard: without it, a machine with only the Vulkan runtime would fail
# to configure instead of falling back to the prebuilt SPIR-V.
if(TARGET NFBasic3DDepthShaders)
    add_dependencies(NFPlayer
        NFBasic3DDepthShaders
        NFBasic3DGBufferShaders
        NFBasic3DLightingShaders
        NFBasic3DForwardShaders
        NFBasic3DTonemapShaders
        NFBasic3DBloomShaders
        NFBasic3DSSAOShaders
        NFBasic3DSSAOBlurShaders
        NFBasic3DPickShaders
        NFBasic3DPresentShaders)
endif()

if(MSVC)
    set_property(TARGET NFPlayer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS NFPlayer DESTINATION bin)
