// MiniAudioImpl.cpp — the single translation unit that compiles the
// vendored miniaudio implementation (decode-only).
//
// MA_NO_ENCODING drops the encoders (the engine never writes audio files).
// Device I/O is not referenced; miniaudio never opens hardware here — it is
// purely a format decoder feeding AudioBuffer.

#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
