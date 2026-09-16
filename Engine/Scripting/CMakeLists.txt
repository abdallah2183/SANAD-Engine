# NFScripting — Lua 5.4 gameplay scripting (VM + entity bindings + system).
#
# Depends on NFEcs/NFScene for the entity bindings (transform access);
# the VM core itself only needs NFCore. The vendored Lua static library
# links privately: no lua headers leak to dependents.

set(NFSCRIPTING_SOURCES
    src/ScriptEngine.cpp
    src/EntityBindings.cpp
    src/ScriptSystem.cpp
)

nf_engine_library(NFScripting
    SOURCES ${NFSCRIPTING_SOURCES}
    DEPENDS NFCore NFEcs NFScene
)

target_link_libraries(NFScripting PRIVATE lua)
