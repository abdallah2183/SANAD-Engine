# NFCore — Foundation library
# Memory, Math, Containers, Logging, Threading, Types, UUID, Hashing, Time, File System

set(NFCORE_SOURCES
    src/Logger.cpp
    src/Assert.cpp
    src/Types.cpp
    src/Math.cpp
    src/UUID.cpp
    src/Hash.cpp
    src/Time.cpp
    src/FileSystem.cpp
    src/Threading.cpp
    src/Memory.cpp
    src/Containers.cpp
    src/Reflection.cpp
    src/Profiler.cpp
)

nf_engine_library(NFCore
    SOURCES ${NFCORE_SOURCES}
)

# Platform-specific sources
if(NF_PLATFORM_WINDOWS)
    target_sources(NFCore PRIVATE src/Platform/Windows/FileSystem_Win.cpp)
endif()
