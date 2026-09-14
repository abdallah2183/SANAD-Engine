# NFProjectTool — logic behind the `nf` CLI and the asset cooker.
#
# Kept in a static library rather than inside main.cpp so it is unit-testable.
# The editor does the same thing (NFEditorCore + a thin exe) and that is the only
# reason its panels have real test coverage.

add_library(NFProjectTool STATIC
    src/ProjectCooker.cpp
    src/ProjectScaffold.cpp
    src/ProjectPackager.cpp
)

target_include_directories(NFProjectTool
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(NFProjectTool PUBLIC NFCore NFAssets)

nf_target_set_warnings(NFProjectTool)

if(MSVC)
    set_property(TARGET NFProjectTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
