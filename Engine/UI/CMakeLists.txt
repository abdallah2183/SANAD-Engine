# NFUI — gameplay/editor UI support library.
#
# Starts with Arabic text shaping + UI localisation (Phase 15); the runtime
# UI framework (widgets, layout, game HUD) builds on this module later.

set(NFUI_SOURCES
    src/ArabicShaper.cpp
    src/Localization.cpp
)

nf_engine_library(NFUI
    SOURCES ${NFUI_SOURCES}
    DEPENDS NFCore
    DIR UI
)
