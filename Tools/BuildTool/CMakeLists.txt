# nf — the NOVAForge project CLI
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
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(MSVC)
    set_property(TARGET nf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS nf DESTINATION bin)
