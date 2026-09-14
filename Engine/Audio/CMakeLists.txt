# NFAudio — audio engine and mixer.
#
# The design document plans MiniAudio integration. MiniAudio is a genuinely
# lightweight single-header library and is the right long-term choice, but the
# engine-facing API is designed first and tested headless via NullAudioDevice.
# All audio math (attenuation, 3D positioning, mixing) is testable without a
# sound card, and CI stays green on headless runners.

set(NFAUDIO_SOURCES
    src/AudioEngine.cpp
)

nf_engine_library(NFAudio
    SOURCES ${NFAUDIO_SOURCES}
    DEPENDS NFCore
)
