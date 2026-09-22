# NFAudio — audio engine and mixer.
#
# Output goes through the AudioDevice seam: NullAudioDevice (silence,
# headless/CI) or WasapiAudioDevice (real shared-mode output on Windows).
# The import pipeline (WAV/OGG/MP3/FLAC -> AudioBuffer) is backed by the
# vendored miniaudio single-header in decode-only mode; miniaudio never
# touches hardware here. All audio math (attenuation, 3D positioning,
# mixing) is testable without a sound card, and CI stays green on runners
# with no audio hardware because every backend degrades to silence, loudly
# logged, never as a failure.
#
# AudioScene.hpp composes the shipped pieces (reverb zones, occlusion, music
# /ambience, buses + volume settings) into one block-based mix, so a game
# level's audio is authoring + three calls rather than an integration project.

set(NFAUDIO_SOURCES
    src/AudioEngine.cpp
    src/AudioDecoder.cpp
    src/AudioScene.cpp
    src/Buses.cpp
    src/MiniAudioImpl.cpp
    src/MusicSystem.cpp
    src/Occlusion.cpp
    src/OggVorbisImpl.cpp
    src/Reverb.cpp
)

if(WIN32)
    list(APPEND NFAUDIO_SOURCES src/Windows/WasapiAudioDevice.cpp)
endif()

nf_engine_library(NFAudio
    SOURCES ${NFAUDIO_SOURCES}
    DEPENDS NFCore
)

# The vendored miniaudio implementation is third-party code: it is warning-clean
# at /W1 but not under the engine's /W4 /WX policy, so this one TU opts out.
# (Same rule as ThirdParty/imgui and ThirdParty/lua, applied per-file because
# the implementation must live inside NFAudio.)
set_source_files_properties(src/MiniAudioImpl.cpp PROPERTIES COMPILE_OPTIONS "/W1;/WX-")
# stb_vorbis is conversion-heavy by design (C4242); silence it for this TU.
set_source_files_properties(src/OggVorbisImpl.cpp PROPERTIES COMPILE_OPTIONS "/W1;/WX-;/wd4242")

target_link_libraries(NFAudio PUBLIC miniaudio)

# stb_vorbis include dir (OGG backend). The header-only declarations ride in
# AudioDecoder.cpp; the implementation TU below stays warning-lenient.
target_include_directories(NFAudio PRIVATE ${CMAKE_SOURCE_DIR}/ThirdParty/stb)

if(WIN32)
    # COM + multimedia device APIs for the WASAPI backend.
    target_link_libraries(NFAudio PUBLIC ole32)
endif()
