# NFScene — Scene / Transform Hierarchy / Prefab

set(NFSCENE_SOURCES
    src/Transform.cpp
    src/StreamingVolume.cpp
    src/Scene.cpp
    src/Prefab.cpp
)

nf_engine_library(NFScene
    SOURCES ${NFSCENE_SOURCES}
    DEPENDS NFCore NFEcs
)

target_include_directories(NFScene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
