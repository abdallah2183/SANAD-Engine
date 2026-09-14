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

if(MSVC)
    set_property(TARGET NFPlayer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS NFPlayer DESTINATION bin)
