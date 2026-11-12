# NFPlayer — the standalone game runtime.
#
# Deliberately thin: it parses arguments, reads a project descriptor, and hands
# off to nf::runtime::Application. Everything that makes a game run already lives
# in NFRuntime; the only thing this executable adds is "start from a project
# instead of from the engine source tree".

add_executable(NFPlayer
    main.cpp
)

set(player_targets NFPlayer)
if(WIN32)
    # The CLI player retains console I/O. Exports use a native Windows
    # application built from the same runtime and argument parser.
    add_executable(NFGameExport WIN32 main.cpp EmbeddedGame.cpp)
    target_link_options(NFGameExport PRIVATE /ENTRY:mainCRTStartup)
    target_compile_definitions(NFGameExport PRIVATE NF_EMBEDDED_GAME_PLAYER)
    target_link_libraries(NFGameExport PRIVATE NFProjectTool ole32 gdi32 user32)
    list(APPEND player_targets NFGameExport)

    set(NF_PLAYER_ICON "${CMAKE_SOURCE_DIR}/Editor/resources/SANAD.ico")
    configure_file(player.rc.in "${CMAKE_CURRENT_BINARY_DIR}/player.rc" @ONLY)
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/player.rc"
                 APPEND PROPERTY OBJECT_DEPENDS "${NF_PLAYER_ICON}")
    if(TARGET nf)
        add_dependencies(nf NFGameExport NFPlayer)
    endif()
endif()

# NFRuntime bakes NF_BASIC3D_SHADER_DIR in and loads the Basic3D pipelines
# from it at startup, so a player built without the compiled SPIR-V present
# starts up with no shaders and renders nothing. The same edge as the editor
# and the samples declare (Editor/CMakeLists.txt, Samples/CMakeLists.txt).
#
# The targets only exist when glslc was found at configure time, hence the
# TARGET guard: without it, a machine with only the Vulkan runtime would fail
# to configure instead of falling back to the prebuilt SPIR-V.
foreach(player IN LISTS player_targets)
    target_link_libraries(${player} PRIVATE NFCore NFAssets NFRuntime)
    nf_target_set_warnings(${player})
    target_compile_definitions(${player} PRIVATE "NF_ENGINE_VERSION=\"${NF_VERSION_STRING}\"")
    if(WIN32)
        target_sources(${player} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/player.rc")
    endif()
    if(TARGET NFGameUiAssets)
        add_dependencies(${player} NFGameUiAssets)
    endif()
    if(TARGET NFBasic3DDepthShaders)
        add_dependencies(${player}
            NFBasic3DDepthShaders
            NFBasic3DGBufferShaders
            NFBasic3DLightingShaders
            NFBasic3DForwardShaders
            NFBasic3DTonemapShaders NFBasic3DFXAAShaders
            NFBasic3DBloomShaders
            NFBasic3DSSAOShaders
            NFBasic3DSSAOBlurShaders
            NFBasic3DPickShaders
            NFBasic3DPresentShaders)
    endif()
    if(MSVC)
        set_property(TARGET ${player} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    install(TARGETS ${player} DESTINATION bin)
endforeach()
