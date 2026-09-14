# NFAssetCooker — Asset cooking tool

add_executable(NFAssetCooker
    main.cpp
)

target_link_libraries(NFAssetCooker PRIVATE NFCore NFAssets NFProjectTool)
nf_target_set_warnings(NFAssetCooker)

target_include_directories(NFAssetCooker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    set_property(TARGET NFAssetCooker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Install to bin
install(TARGETS NFAssetCooker DESTINATION bin)
