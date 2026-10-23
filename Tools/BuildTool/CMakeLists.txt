# nf — the SANAD project CLI
#
# Thin front end over NFProjectTool. It needs to know where the project template
# and the compiled renderer shaders live, because it is the tool that turns a
# project into something that runs outside the engine tree; both are passed as
# compile definitions, the same way NFRuntime receives NF_BASIC3D_SHADER_DIR.

add_executable(nf
    main.cpp
)

target_link_libraries(nf PRIVATE NFCore NFAssets NFProjectTool)
nf_target_set_warnings(nf)

target_compile_definitions(nf PRIVATE
    "NF_TEMPLATE_DIR=\"${CMAKE_SOURCE_DIR}/Templates/Default\""
    "NF_TEMPLATES_ROOT=\"${CMAKE_SOURCE_DIR}/Templates\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

# `nf` bakes NF_BASIC3D_SHADER_DIR into the binary and copies that directory
# into every package it produces, so the compiled SPIR-V has to exist before
# the tool is built. Without this edge, building only `nf` leaves the tree
# with an empty shader directory: `nf build` then packages a game with no
# shaders, and the shipped game launches to a black screen with no error at
# all. Every other consumer of the same SPIR-V declares the dependency (see
# Editor/CMakeLists.txt and Samples/CMakeLists.txt) — these two were the gap.
#
# The targets only exist when glslc was found at configure time, hence the
# TARGET guard: without it, a machine with only the Vulkan runtime would fail
# to configure instead of falling back to the prebuilt SPIR-V.
if(TARGET NFBasic3DDepthShaders)
    add_dependencies(nf
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
    set_property(TARGET nf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS nf DESTINATION bin)
