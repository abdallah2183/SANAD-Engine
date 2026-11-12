#pragma once

// NF/Audio/WasapiAudioDevice.hpp — real audio output on Windows (WASAPI shared mode).
//
// The engine mixes on its own clock (Runtime::step_audio) while WASAPI pulls
// on the OS mixer clock, so the two sides meet in AudioRingBuffer below: the
// mix thread pushes planar float, the render thread pops interleaved float.
// Single producer, single consumer, wait-free in the common path (two atomics,
// no mutex, no allocation after construction).
//
// Failure policy: every HRESULT is checked, and ANY failure makes initialize()
// return false so the caller falls back to NullAudioDevice. Silence beats a
// crash, a hang, or a half-opened endpoint — especially on CI runners and
// servers with no audio hardware at all.

#ifdef _WIN32

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/AudioRingBuffer.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace nf::audio {

/// WASAPI shared-mode output (event-driven). One instance per process is the
/// norm; the class is nevertheless safe to construct repeatedly because every
/// OS handle is owned and released symmetrically, and shutdown() is idempotent.
class WasapiAudioDevice : public AudioDevice {
public:
    WasapiAudioDevice() = default;
    ~WasapiAudioDevice() override { shutdown(); }

    WasapiAudioDevice(const WasapiAudioDevice&) = delete;
    WasapiAudioDevice& operator=(const WasapiAudioDevice&) = delete;

    bool initialize(u32 sample_rate, u32 buffer_frames) override;
    void shutdown() override;
    // Unlocked core shared by shutdown() and re-entrant initialize().
    // Callers hold m_state_mutex.
    void shutdown_impl();
    bool is_initialized() const override { return m_initialized.load(std::memory_order_acquire); }
    u32 sample_rate() const override { return m_sample_rate; }
    void request_buffer(f32* left, f32* right, usize num_frames) override;
    const char* backend_name() const override { return "wasapi-shared"; }
    bool accepts_push() const override { return true; }
    void submit_mix(const f32* left, const f32* right, usize num_frames) override;

    u64 frames_submitted() const { return m_submitted.load(std::memory_order_relaxed); }
    u64 frames_rendered() const { return m_rendered.load(std::memory_order_relaxed); }
    u64 underruns() const { return m_ring.underruns(); }
    u64 overruns() const { return m_ring.overruns(); }
    /// Endpoint mix rate (what the ring and the render thread run at). Equals
    /// the initialize() rate when the endpoint takes it directly.
    u32 mix_rate() const { return m_mix_rate; }

private:
    void render_loop();
    void release_com() noexcept;

    std::atomic<bool> m_initialized{false};
    u32 m_sample_rate = 0;

    // COM objects as void* so <Audioclient.h> stays in the .cpp (this header
    // is included by tests and must stay light).
    void* m_client = nullptr;        // IAudioClient
    void* m_render_client = nullptr; // IAudioRenderClient
    void* m_event = nullptr;         // HANDLE, event-callback mode
    u32 m_endpoint_buffer_frames = 0;
    bool m_com_initialized = false;

    AudioRingBuffer m_ring;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<u64> m_submitted{0};
    std::atomic<u64> m_rendered{0};
    std::mutex m_state_mutex; // serializes initialize/shutdown only
    // Resample state (submit thread only): fractional position carried
    // across submit_mix calls, plus scratch for the converted block.
    u32 m_mix_rate = 0;
    double m_resample_pos = 0.0;
    std::vector<f32> m_resample_scratch_l;
    std::vector<f32> m_resample_scratch_r;
};

} // namespace nf::audio

#endif // _WIN32
