# NFUI — gameplay/editor UI support library.
#
# Arabic text shaping + UI localisation (Phase 15), plus the RUNTIME game UI
# (Game-Ready G3): retained widgets + display list (Widgets) and the shipped-
# game spine — HUD, menus, dialogue presentation, screen flow (GameUI).
#
# NFGameplay is a PUBLIC dependency because GameUI's dialogue presenter wraps
# gameplay::DialogueRunner (data layer). NFGameplay itself depends only on
# NFCore, so there is no cycle.

set(NFUI_SOURCES
    src/ArabicShaper.cpp
    src/Localization.cpp
    src/Widgets.cpp
    src/GameUI.cpp
)

nf_engine_library(NFUI
    SOURCES ${NFUI_SOURCES}
    DEPENDS NFCore NFGameplay
    DIR UI
)
