# NFModelImporter — glTF 2.0 -> .nfmesh conversion tool

add_executable(NFModelImporter
    main.cpp
)

target_link_libraries(NFModelImporter PRIVATE NFCore NFAssets)
nf_target_set_warnings(NFModelImporter)

if(MSVC)
    set_property(TARGET NFModelImporter PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Install to bin
install(TARGETS NFModelImporter DESTINATION bin)
