# NFRuntime — Application + Runtime + SceneLoader

set(NFRUNTIME_SOURCES
    src/Application.cpp
    src/RunConfigResolver.cpp
    src/Runtime.cpp
    src/RuntimeSceneLoader.cpp
    src/SceneExtraction.cpp
    src/WorldStreamer.cpp
    src/SaveSystem.cpp
    src/InputLog.cpp
    src/BuiltinModules.cpp
    src/PlayerControllerModule.cpp
    src/OrbitCameraModule.cpp
    src/KeyboardInput.cpp
    src/InputBindings.cpp
    src/CameraFollow.cpp
    src/PlayerLook.cpp
    src/PlayerSettings.cpp
    src/SceneValidator.cpp
    src/CharacterRuntime.cpp
    src/Weapon.cpp
    src/Health.cpp
    src/Damage.cpp
    src/Explosion.cpp
    src/Hitscan.cpp
    src/Projectile.cpp
    src/Factions.cpp
    src/Impact.cpp
    src/Shot.cpp
    src/StatusEffect.cpp
    src/DamageLedger.cpp
    src/Suppression.cpp
    src/WeaponRuntime.cpp
    src/CombatAI.cpp
)

nf_engine_library(NFRuntime
    SOURCES ${NFRUNTIME_SOURCES}
    DEPENDS NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets NFPhysics NFDestruction NFAnimation NFAudio NFGameplay NFScripting NFVfx NFAI
)

target_include_directories(NFRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(NFRuntime PRIVATE "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\"")
# The save format stamps the engine version into every slot's meta.txt. Taken
# from the CMake project version so the stamp cannot drift from the binary.
target_compile_definitions(NFRuntime PRIVATE "NF_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
