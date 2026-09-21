# NFScene2D — 2D Engine: sprites, tilemaps, 2D physics, particles
# Design doc Section 54 (2D Engine), Section 55 (Tilemap), Section 56 (2D Physics).

set(NFSCENE2D_SOURCES
    src/SpriteAtlas.cpp
    src/SpriteBatcher.cpp
    src/Tilemap.cpp
    src/Physics2D.cpp
    src/Particles2D.cpp
    src/Light2D.cpp
    src/Systems.cpp
)

nf_engine_library(NFScene2D
    SOURCES ${NFSCENE2D_SOURCES}
    DEPENDS NFCore NFEcs NFScene
)

target_include_directories(NFScene2D PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
