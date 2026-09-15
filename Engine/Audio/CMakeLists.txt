# NFAudio — audio engine and mixer.
#
# Output goes through the AudioDevice seam: NullAudioDevice (silence,
# headless/CI) or WasapiAudioDevice (real shared-mode output on Windows).
# A third-party backend (MiniAudio was once considered) would add a vendored
# dependency for what the OS mixer already does; the seam keeps that option
# open without requiring it. All audio math (attenuation, 3D positioning,
# mixing) is testable without a sound card, and CI stays green on runners
# with no audio hardware because every backend degrades to silence, loudly
# logged, never as a failure.

set(NFAUDIO_SOURCES
    src/AudioEngine.cpp
)

if(WIN32)
    list(APPEND NFAUDIO_SOURCES src/Windows/WasapiAudioDevice.cpp)
endif()

nf_engine_library(NFAudio
    SOURCES ${NFAUDIO_SOURCES}
    DEPENDS NFCore
)

if(WIN32)
    # COM + multimedia device APIs for the WASAPI backend.
    target_link_libraries(NFAudio PUBLIC ole32)
endif()
