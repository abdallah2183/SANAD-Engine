# NFScripting — Lua 5.4 gameplay scripting (VM + entity bindings + system).
#
# Depends on NFEcs/NFScene for the entity bindings (transform access);
# the VM core itself only needs NFCore. The vendored Lua static library
# links privately: no lua headers leak to dependents.

# NFScripting — Lua 5.4 gameplay scripting (VM + entity bindings + system)
# plus optional C# scripting through .NET hosting (hostfxr + managed sandbox).
#
# Depends on NFEcs/NFScene for the entity bindings (transform access);
# the VM core itself only needs NFCore. The vendored Lua static library
# links privately: no lua headers leak to dependents.
#
# C# hosting needs the .NET SDK (for the managed sandbox build) and the
# nethost pack (for hostfxr discovery). Both missing => the host compiles to
# a loud stub (CSharpHost::initialize fails with an explanation) and the
# hosting tests skip; pure-logic marshaling always builds.

set(NFSCRIPTING_SOURCES
    src/ScriptEngine.cpp
    src/EntityBindings.cpp
    src/ScriptSystem.cpp
    src/UiBindings.cpp
    src/CSharpMarshal.cpp
    src/CSharpHost.cpp
)

nf_engine_library(NFScripting
    SOURCES ${NFSCRIPTING_SOURCES}
    DEPENDS NFCore NFEcs NFScene NFUI
)

target_link_libraries(NFScripting PRIVATE lua)

# --- C# hosting (optional, Windows + .NET SDK) ---
set(NF_CSHARP_HOSTING OFF)
if(WIN32)
    find_program(DOTNET_EXE dotnet DOC "C# SDK for the managed scripting sandbox")
    if(DOTNET_EXE)
        get_filename_component(_nf_dotnet_dir "${DOTNET_EXE}" DIRECTORY)
        file(GLOB _nf_nethost_libs
            "${_nf_dotnet_dir}/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native/nethost.lib")
        # Glob sorts alphabetically ("10.x" < "8.x"): compare as versions and
        # take the newest, so the hostfxr can always load the sandbox TFM.
        set(NF_NETHOST_LIB "")
        set(_nf_nethost_ver "0.0.0")
        foreach(_nf_lib IN LISTS _nf_nethost_libs)
            string(REGEX MATCH "Host\\.win-x64/([^/]+)/" _nf_ver_match "${_nf_lib}")
            if(CMAKE_MATCH_1 VERSION_GREATER _nf_nethost_ver)
                set(_nf_nethost_ver "${CMAKE_MATCH_1}")
                set(NF_NETHOST_LIB "${_nf_lib}")
            endif()
        endforeach()
        if(NF_NETHOST_LIB)
            get_filename_component(NF_NETHOST_DIR "${NF_NETHOST_LIB}" DIRECTORY)
            set(NF_CSHARP_HOSTING ON)
        endif()
    endif()
endif()

if(NF_CSHARP_HOSTING)
    message(STATUS "[NF] C# hosting enabled (nethost: ${NF_NETHOST_DIR})")
    target_compile_definitions(NFScripting PRIVATE NF_CSHARP_HOSTING=1)
    target_compile_definitions(NFScripting PRIVATE NF_NETHOST_DIR="${NF_NETHOST_DIR}")
    target_include_directories(NFScripting PRIVATE "${NF_NETHOST_DIR}")
    target_link_libraries(NFScripting PRIVATE "${NF_NETHOST_LIB}")

    # Managed sandbox: dependency-free test assembly + runtimeconfig, built
    # with the SDK into the binary dir (source tree stays pristine).
    set(NF_CSHARP_OUT "${CMAKE_CURRENT_BINARY_DIR}/csharp")
    set(NF_CSHARP_DLL "${NF_CSHARP_OUT}/NFGameScript.dll")
    # Every managed source is a dependency, not just the first one written: an
    # edit to any other .cs (e.g. NFUiDriver.cs) must re-run this command, or the
    # sandbox DLL silently goes stale and the C# tests exercise old code.
    file(GLOB NF_CSHARP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/CSharp/NFGameScript/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/CSharp/NFGameScript/*.csproj")
    add_custom_command(
        OUTPUT "${NF_CSHARP_DLL}" "${NF_CSHARP_OUT}/NFGameScript.runtimeconfig.json"
        COMMAND "${DOTNET_EXE}" build
            "${CMAKE_CURRENT_SOURCE_DIR}/CSharp/NFGameScript/NFGameScript.csproj"
            -c "$<CONFIG>" --nologo -v q -o "${NF_CSHARP_OUT}"
        DEPENDS ${NF_CSHARP_SOURCES}
        COMMENT "Building managed C# sandbox (NFGameScript)"
        VERBATIM
    )
    add_custom_target(NFCSharpSandbox ALL DEPENDS "${NF_CSHARP_DLL}")
    # Visible to Tests/ (sibling scope): managed output dir + hosting flag.
    set(NF_CSHARP_OUT "${NF_CSHARP_OUT}" PARENT_SCOPE)
    set(NF_CSHARP_HOSTING "${NF_CSHARP_HOSTING}" PARENT_SCOPE)
else()
    message(STATUS "[NF] C# hosting disabled (needs Windows + dotnet + nethost)")
    target_compile_definitions(NFScripting PRIVATE NF_CSHARP_HOSTING=0)
endif()
