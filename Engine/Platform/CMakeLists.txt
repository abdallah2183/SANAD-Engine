# NFPlatform — Platform abstraction (Window, Input, OS)

set(NFPLATFORM_SOURCES
    src/Platform.cpp
    src/Window.cpp
    src/InputSystem.cpp
    src/Replay.cpp
)

if(NF_PLATFORM_WINDOWS)
    list(APPEND NFPLATFORM_SOURCES
        src/Windows/Window_Win.cpp
        src/Windows/InputSystem_Win.cpp
        src/Windows/Platform_Win.cpp
    )
endif()

nf_engine_library(NFPlatform
    SOURCES ${NFPLATFORM_SOURCES}
    DEPENDS NFCore NFJobs
)
