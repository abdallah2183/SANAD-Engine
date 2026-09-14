# NFGameplay — the C++ gameplay layer.
#
# Deliberately thin. It owns the module lifecycle contract, the registry that
# makes a module discoverable without the engine naming it, and the reflected
# state bridge that lets a module's settings be saved and inspected. It does NOT
# own the Runtime (the Runtime owns modules), so there is no dependency from here
# back into Engine/Runtime — a module that needs engine services gets them
# through GameplayContext.

set(NFGAMEPLAY_SOURCES
    src/GameplayModuleRegistry.cpp
    src/GameplayState.cpp
)

nf_engine_library(NFGameplay
    SOURCES ${NFGAMEPLAY_SOURCES}
    DEPENDS NFCore
)
