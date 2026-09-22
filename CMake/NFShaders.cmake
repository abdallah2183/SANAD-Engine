# NFShaders.cmake
# GLSL → SPIR-V compilation, shared by samples and tests.
#
# Shaders are compiled at build time so nothing needs a shader compiler at
# runtime. When glslc is unavailable the prebuilt .spv files checked into the
# repository are used instead, which keeps the tree buildable on a machine with
# only the Vulkan runtime installed.
#
# Defines, in the calling scope:
#   NF_GLSLC_EXECUTABLE        — path to glslc, or empty
#   NF_TRIANGLE_SHADER_DIR     — directory the Triangle sample reads SPIR-V from
#   NF_QUAD_SHADER_DIR         — directory the TexturedQuad sample + tests read

find_program(NF_GLSLC_EXECUTABLE
    NAMES glslc glslc.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "glslc SPIR-V compiler"
)

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
    # GPU picking id pass (GpuPicker). Deliberately lives beside the other
    # renderer shaders: it reuses their vertex layout and transform path, and
    # picking is only correct if the id image lines up with the rendered one.
    nf_compile_shaders(NFBasic3DPickShaders
        "${NF_BASIC3D_SHADER_SRC_DIR}" "${NF_BASIC3D_SHADER_OUT_DIR}" "pick")

    set(NF_TRIANGLE_SHADER_DIR "${NF_TRIANGLE_SHADER_OUT_DIR}")
    set(NF_QUAD_SHADER_DIR     "${NF_QUAD_SHADER_OUT_DIR}")
    set(NF_RHI_PUSH_SHADER_DIR "${NF_RHI_PUSH_SHADER_OUT_DIR}")
    set(NF_BASIC3D_SHADER_DIR  "${NF_BASIC3D_SHADER_OUT_DIR}")
    nf_compile_shaders(NFEditorImGuiShaders
        "${NF_EDITOR_IMGUI_SHADER_SRC_DIR}" "${NF_EDITOR_IMGUI_SHADER_OUT_DIR}" "imgui")
    set(NF_EDITOR_IMGUI_SHADER_DIR "${NF_EDITOR_IMGUI_SHADER_OUT_DIR}")
else()
    message(STATUS "[NF] glslc not found — using prebuilt SPIR-V from the source tree")
    set(NF_TRIANGLE_SHADER_DIR "${NF_TRIANGLE_SHADER_SRC_DIR}")
    set(NF_QUAD_SHADER_DIR     "${NF_QUAD_SHADER_SRC_DIR}")
    set(NF_RHI_PUSH_SHADER_DIR "${NF_RHI_PUSH_SHADER_SRC_DIR}")
    set(NF_BASIC3D_SHADER_DIR  "${NF_BASIC3D_SHADER_SRC_DIR}")
    set(NF_EDITOR_IMGUI_SHADER_DIR "${NF_EDITOR_IMGUI_SHADER_SRC_DIR}")
endif()
