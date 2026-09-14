# NFCompilerFlags.cmake
# Compiler flags for NOVAForge Engine — strict warnings, fast code, no junk.

set(NF_WARNING_FLAGS_MSVC
    /W4        # High warning level
    /permissive-  # Strict C++
    /utf-8     # Source charset
    /Zc:__cplusplus  # Report correct __cplusplus
    /Zc:preprocessor  # Conforming preprocessor
    /EHsc      # C++ exception handling with C-externs no-throw
    /w14242    # Conversion warnings
    /w14254    # Operator conversion warnings
    /w14263    # Member function override warnings
    /w14265    # Class has virtuals, destructor is not virtual
    /w14287    # Unsigned/signed mismatch
    /w14296    # Expression is always false
    /w14311    # Pointer truncation
    /w14545    # Function call before evaluation
    /w14619    # constexpr not generated
    /w14263    # Override
    /w14189    # Local variable uninitialized
)

set(NF_WARNING_FLAGS_CLANG
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wzero-as-null-pointer-constant
    -Wnull-dereference
    -Wdouble-promotion
    -Wshadow
    -Wformat=2
    -Wimplicit-fallthrough
    -Wno-unknown-pragmas
)

set(NF_WARNING_FLAGS_GCC ${NF_WARNING_FLAGS_CLANG})

function(nf_target_set_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE ${NF_WARNING_FLAGS_MSVC})
        target_compile_definitions(${target_name} PUBLIC NOMINMAX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target_name} PRIVATE ${NF_WARNING_FLAGS_CLANG})
    else()
        target_compile_options(${target_name} PRIVATE ${NF_WARNING_FLAGS_GCC})
    endif()

    if(NF_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /WX)
        else()
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Helper: define an engine library with standard settings.
# Arguments:
#   SOURCES  — list of source files (relative to the module's CMakeLists.txt)
#   HEADERS  — list of header files (optional, for IDE)
#   DEPENDS  — list of target names to link publicly
#   DIR      — subdirectory name under Engine/ (defaults to stripping the NF prefix
#              from the target name: NFCore→Core, NFJobs→Jobs, etc.)
function(nf_engine_library name)
    cmake_parse_arguments(ARG "STATIC;SHARED" "DIR" "SOURCES;HEADERS;DEPENDS" ${ARGN})

    set(lib_type STATIC)
    if(ARG_SHARED)
        set(lib_type SHARED)
    endif()

    # Resolve the Engine subdirectory name
    if(ARG_DIR)
        set(dir_name ${ARG_DIR})
    else()
        # Strip "NF" prefix: NFCore→Core, NFPlatform→Platform, NFRHI→RHI
        string(REGEX REPLACE "^NF" "" dir_name ${name})
    endif()

    set(module_dir "${CMAKE_SOURCE_DIR}/Engine/${dir_name}")

    add_library(${name} ${lib_type}
        ${ARG_SOURCES}
    )

    # Include directories — public headers for dependents, private for internal
    target_include_directories(${name}
        PUBLIC
            ${module_dir}/include
        PRIVATE
            ${module_dir}/src
    )

    # Dependencies
    if(ARG_DEPENDS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    endif()

    nf_target_set_warnings(${name})

    # C++ standard
    target_compile_features(${name} PUBLIC cxx_std_23)

    # MSVC: static runtime
    if(MSVC)
        set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # NOMINMAX: prevent Windows.h from defining min/max macros that clash
        # with std::min/std::max. Required project-wide.
        target_compile_definitions(${name} PUBLIC NOMINMAX)
    endif()
endfunction()
