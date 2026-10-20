# NFShaders.cmake
# GLSL → SPIR-V compilation, shared by samples and tests.
#
# Shaders are compiled at build time so nothing needs a shader compiler at
# runtime. glslc ships with the Vulkan SDK, so the only real requirement is
# that the SDK is installed.
#
# THE COMPILER IS MANDATORY. An earlier version of this file fell back to
# "prebuilt SPIR-V checked into the repository" when glslc could not be found.
# That fallback was a lie: `*.spv` is in .gitignore, so no .spv file has ever
# been committed, and the fallback pointed the shader directories at the GLSL
# *sources*. A build on a machine where the SDK lived at the default
# C:\VulkanSDK location without VULKAN_SDK set and without glslc on PATH
# therefore configured successfully, compiled, linked, and produced an engine
# that loaded no shaders at all — a silent black screen with no error anywhere.
# A missing compiler is now a hard configure error: failing loudly at
# configure time is the only outcome that tells the user what is wrong.
#
# Defines, in the calling scope:
#   NF_GLSLC_EXECUTABLE        — path to glslc
#   NF_TRIANGLE_SHADER_DIR     — directory the Triangle sample reads SPIR-V from
#   NF_QUAD_SHADER_DIR         — directory the TexturedQuad sample + tests read

find_program(NF_GLSLC_EXECUTABLE
    NAMES glslc glslc.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "glslc SPIR-V compiler"
)

# The Vulkan SDK's default install root is C:\VulkanSDK\<version>. Engine/RHI
# finds the SDK headers there when VULKAN_SDK is not set, so glslc has to be
# found there too — otherwise the headers and the compiler come from different
# places and only one of them works. The newest version wins, matching the
# header lookup in Engine/RHI/CMakeLists.txt.
if(NOT NF_GLSLC_EXECUTABLE AND WIN32 AND IS_DIRECTORY "C:/VulkanSDK")
    file(GLOB _nf_vulkan_versions RELATIVE "C:/VulkanSDK" "C:/VulkanSDK/*")
    set(_nf_best_sdk_version "")
    foreach(_v IN LISTS _nf_vulkan_versions)
        if(IS_DIRECTORY "C:/VulkanSDK/${_v}" AND "${_v}" STRGREATER "${_nf_best_sdk_version}")
            set(_nf_best_sdk_version "${_v}")
        endif()
    endforeach()
    if(NOT _nf_best_sdk_version STREQUAL "")
        find_program(NF_GLSLC_EXECUTABLE
            NAMES glslc glslc.exe
            HINTS "C:/VulkanSDK/${_nf_best_sdk_version}/Bin"
            DOC "glslc SPIR-V compiler"
        )
    endif()
endif()

# nf_compile_shaders(<target-name> <source-dir> <output-dir> <base-name>)
#
# Compiles <base-name>.vert and <base-name>.frag into
# <output-dir>/<base-name>_<stage>.spv and creates a custom target named
# <target-name> that depends on both.
#
# Every *.glsl file in the source directory is added as a dependency of both
# outputs. A #included shader (brdf.glsl, the shared lighting body) is part of
# every includer's real input, and glslc resolves the include relative to the
# including file — but ninja only knows the DEPENDS list, so without this a
# brdf.glsl edit leaves the .spv stale and every consumer silently rebuilds
# nothing while believing it rebuilt the light. GLOBbed once at configure
# time: a NEW shared include needs a reconfigure to be picked up, which is the
# acceptable trade against a per-build depfile scan.
#
# Consumers add_dependencies() on that target so their shaders exist before
# they run. The output directory is where the compiled SPIR-V lands; the
# caller passes it to the executable as a compile definition.
function(nf_compile_shaders target_name src_dir out_dir base_name)
    set(_spv_files "")
    file(GLOB _glsl_includes "${src_dir}/*.glsl")

    foreach(_stage vert frag)
        set(_src "${src_dir}/${base_name}.${_stage}")
        set(_spv "${out_dir}/${base_name}_${_stage}.spv")

        add_custom_command(
            OUTPUT  "${_spv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
            COMMAND "${NF_GLSLC_EXECUTABLE}" -fshader-stage=${_stage} "${_src}" -o "${_spv}"
            DEPENDS "${_src}" ${_glsl_includes}
            COMMENT "Compiling ${_src} -> ${_spv}"
            VERBATIM
        )
        list(APPEND _spv_files "${_spv}")
    endforeach()

    add_custom_target(${target_name} DEPENDS ${_spv_files})
endfunction()

# --- Triangle -------------------------------------------------------------

set(NF_TRIANGLE_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Samples/Triangle/shaders")
set(NF_TRIANGLE_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/Triangle")

# --- Textured quad --------------------------------------------------------

set(NF_QUAD_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Samples/TexturedQuad/shaders")
set(NF_QUAD_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/TexturedQuad")

# --- Push-constant test (RHI) ------------------------------------------------

set(NF_RHI_PUSH_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RHITests/shaders")
set(NF_RHI_PUSH_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/RHIPush")

# --- Basic3D --------------------------------------------------------------

set(NF_BASIC3D_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Samples/Basic3D/shaders")
set(NF_BASIC3D_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/Basic3D")

# --- Editor ImGui overlay ---------------------------------------------------

set(NF_EDITOR_IMGUI_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Editor/shaders")
set(NF_EDITOR_IMGUI_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/EditorImGui")

# --- CliffStory 2D sprite pipeline ------------------------------------------
# The 2D sample's only shader pair: position + uv + tint in, modulated texel
# out. Lives beside the others because the same nf_compile_shaders helper fits
# it exactly — one .vert and one .frag, no includes, no push constants.

set(NF_CLIFF_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Samples/CliffStory/shaders")
set(NF_CLIFF_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders/CliffStory")

if(NF_GLSLC_EXECUTABLE)
    message(STATUS "[NF] glslc found: ${NF_GLSLC_EXECUTABLE}")

    nf_compile_shaders(NFTriangleShaders
        "${NF_TRIANGLE_SHADER_SRC_DIR}" "${NF_TRIANGLE_SHADER_OUT_DIR}" "triangle")
    nf_compile_shaders(NFQuadShaders
        "${NF_QUAD_SHADER_SRC_DIR}" "${NF_QUAD_SHADER_OUT_DIR}" "textured_quad")
    nf_compile_shaders(NFRHIPushShaders
        "${NF_RHI_PUSH_SHADER_SRC_DIR}" "${NF_RHI_PUSH_SHADER_OUT_DIR}" "push_constant")
    nf_compile_shaders(NFBasic3DDepthShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "depth")
    nf_compile_shaders(NFBasic3DGBufferShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "gbuffer")
    nf_compile_shaders(NFBasic3DLightingShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "lighting")
    # The transparency pass. brdf.glsl is not compiled on its own — it is
    # #included by lighting.frag and forward.frag, and glslc resolves the
    # include relative to the including file, so it only has to sit in this
    # directory. One copy of the BRDF for both paths is the reason it exists.
    nf_compile_shaders(NFBasic3DForwardShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "forward")
    nf_compile_shaders(NFBasic3DTonemapShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "tonemap")
    # The bloom chain (§206). One shader pair serves both the prefilter and the
    # downsample; the kernel rides a push-constant mode flag (see bloom.frag).
    nf_compile_shaders(NFBasic3DBloomShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "bloom")
    # SSAO pair: hemisphere occlusion (ssao) + bilateral blur (ssao_blur).
    # Both fullscreen, both optional: without the .spv the stage stays off
    # and the lighting pass reads scalar 1.0.
    nf_compile_shaders(NFBasic3DSSAOShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "ssao")
    nf_compile_shaders(NFBasic3DSSAOBlurShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "ssao_blur")
    # GPU picking id pass (GpuPicker). Deliberately lives beside the other
    # renderer shaders: it reuses their vertex layout and transform path, and
    # picking is only correct if the id image lines up with the rendered one.
    nf_compile_shaders(NFBasic3DPickShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "pick")
    # Present passthrough (texture -> target, no tonemap). Renderer3D's
    # present_texture() uses it to put an already-finished frame image (the
    # editor viewport) on a swapchain without a second full scene render.
    nf_compile_shaders(NFBasic3DPresentShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "present")

    set(NF_TRIANGLE_SHADER_DIR "${NF_TRIANGLE_SHADER_OUT_DIR}")
    set(NF_QUAD_SHADER_DIR     "${NF_QUAD_SHADER_OUT_DIR}")
    set(NF_RHI_PUSH_SHADER_DIR "${NF_RHI_PUSH_SHADER_OUT_DIR}")
    set(NF_BASIC3D_SHADER_DIR  "${NF_BASIC3D_SHADER_OUT_DIR}")
    nf_compile_shaders(NFEditorImGuiShaders
        "${NF_EDITOR_IMGUI_SHADER_SRC_DIR}" "${NF_EDITOR_IMGUI_SHADER_OUT_DIR}" "imgui")
    set(NF_EDITOR_IMGUI_SHADER_DIR "${NF_EDITOR_IMGUI_SHADER_OUT_DIR}")
    nf_compile_shaders(NFCliffStoryShaders
        "${NF_CLIFF_SHADER_SRC_DIR}" "${NF_CLIFF_SHADER_OUT_DIR}" "sprite2d")
    set(NF_CLIFF_SHADER_DIR "${NF_CLIFF_SHADER_OUT_DIR}")
else()
    # No glslc, so no SPIR-V, so no engine. Failing here is the whole point:
    # the previous fallback produced a binary that started, drew nothing and
    # said nothing. See the note at the top of this file.
    message(FATAL_ERROR
        "[NF] glslc (the Vulkan SDK shader compiler) was not found.\n"
        "     The engine's shaders are compiled at build time; there are no "
        "prebuilt .spv files in the repository to fall back on.\n"
        "     Install the LunarG Vulkan SDK (1.1+) and either:\n"
        "       * set the VULKAN_SDK environment variable, or\n"
        "       * install to the default C:\\VulkanSDK location, or\n"
        "       * put glslc on PATH.\n"
        "     (Searched: $ENV{VULKAN_SDK}/Bin, C:/VulkanSDK/<version>/Bin, PATH)")
endif()

