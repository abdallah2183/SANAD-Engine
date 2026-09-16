#pragma once

// NF/Audio/AudioDecoder.hpp — compressed audio import pipeline (Phase 14).
//
// Decodes WAV / OGG-Vorbis / MP3 / FLAC (file or memory) into the engine's
// AudioBuffer (32-bit float PCM). Decoding is backed by the vendored
// miniaudio single-header (decode-only; it never touches audio hardware —
// output stays on the AudioDevice seam).
//
// All functions are synchronous and thread-safe (no shared state). Failure
// yields ok=false plus a human-readable error; nothing throws.

#include <NF/Audio/AudioEngine.hpp>

#include <cstddef>
#include <string>

namespace nf::audio {

/// Container sniffing from magic bytes. Never reads out of bounds; returns
/// Unknown for null/short buffers.
enum class AudioFileFormat : u8 {
    Unknown = 0,
    Wav,
    OggVorbis,
    Mp3,
    Flac,
};

AudioFileFormat detect_audio_format(const void* data, usize size);

/// Human-readable name for a format ("wav", "ogg", "mp3", "flac", "unknown").
const char* audio_format_name(AudioFileFormat format);

struct DecodeOptions {
    /// 0 = keep the file's channel count. Otherwise the PCM is mixed
    /// up/down to this many channels (1 or 2).
    u32 target_channels = 0;
    /// 0 = keep the file's sample rate. Otherwise the PCM is resampled
    /// (linear interpolation).
    u32 target_sample_rate = 0;
};

/// Channel mix + linear resample of decoded PCM. Pure function, no I/O.
/// A no-op (returns src unchanged) when both targets are 0 or already match.
AudioBuffer convert_audio_format(const AudioBuffer& src, u32 target_channels,
                                 u32 target_sample_rate);

struct DecodeResult {
    AudioBuffer buffer;
    AudioFileFormat format = AudioFileFormat::Unknown;
    bool ok = false;
    std::string error;
};

/// Decodes one audio file from memory. Empty/null input fails cleanly.
DecodeResult decode_audio_memory(const void* data, usize size,
                                 const DecodeOptions& options = {});

/// Reads `path` fully, then decodes. Missing/unreadable files fail cleanly.
DecodeResult decode_audio_file(const std::string& path,
                               const DecodeOptions& options = {});

} // namespace nf::audio
