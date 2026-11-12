#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/AudioRingBuffer.hpp>
#include <NF/Core/Logger.hpp>

#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace nf::audio {
namespace {

// Runtime mixes on the game thread; Android consumes the same stereo stream
// on its audio callback thread. The callback only copies from an SPSC ring and
// fills an underrun with silence; it never allocates, locks, or logs.
class AAudioDevice final : public AudioDevice {
public:
    ~AAudioDevice() override { shutdown(); }

    bool initialize(u32 rate, u32 buffer_frames) override {
        shutdown();
        if (rate == 0 || buffer_frames == 0) return false;
        m_sample_rate = rate;
        m_buffer_frames = buffer_frames;
        m_ring = std::make_unique<AudioRingBuffer>(65536);
        m_error.store(AAUDIO_OK, std::memory_order_relaxed);

        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || builder == nullptr) {
            return failed_open();
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(rate));
        AAudioStreamBuilder_setDataCallback(builder, &data_callback, this);
        AAudioStreamBuilder_setErrorCallback(builder, &error_callback, this);
        result = AAudioStreamBuilder_openStream(builder, &m_stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || m_stream == nullptr) {
            NF_LOG_WARN(LogCategory::Audio, "AAudio: opening stream failed ({})", result);
            return failed_open();
        }
        if (AAudioStream_getFormat(m_stream) != AAUDIO_FORMAT_PCM_FLOAT ||
            AAudioStream_getChannelCount(m_stream) != 2 ||
            AAudioStream_getSampleRate(m_stream) != static_cast<int32_t>(rate)) {
            NF_LOG_WARN(LogCategory::Audio, "AAudio: stream format did not match the mixer");
            return failed_open();
        }
        result = AAudioStream_requestStart(m_stream);
        if (result != AAUDIO_OK) {
            NF_LOG_WARN(LogCategory::Audio, "AAudio: starting stream failed ({})", result);
            return failed_open();
        }
        m_initialized = true;
        return true;
    }

    void shutdown() override {
        m_initialized = false;
        if (m_stream) {
            AAudioStream_requestStop(m_stream);
            // After close returns the callback can no longer access m_ring.
            AAudioStream_close(m_stream);
            m_stream = nullptr;
        }
        m_ring.reset();
    }

    bool is_initialized() const override { return m_initialized; }
    u32 sample_rate() const override { return m_sample_rate; }
    const char* backend_name() const override { return "aaudio-shared"; }
    bool accepts_push() const override { return true; }

    void request_buffer(f32* left, f32* right, usize frames) override {
        if (left) std::memset(left, 0, frames * sizeof(f32));
        if (right) std::memset(right, 0, frames * sizeof(f32));
    }

    void submit_mix(const f32* left, const f32* right, usize frames) override {
        const auto error = m_error.load(std::memory_order_acquire);
        if (error != AAUDIO_OK) {
            // Reopen from the game thread, never from AAudio's error callback.
            const auto now = std::chrono::steady_clock::now();
            if (now < m_next_retry) return;
            NF_LOG_WARN(LogCategory::Audio, "AAudio: stream disconnected ({}), reopening", error);
            const u32 rate = m_sample_rate, buffer = m_buffer_frames;
            if (!initialize(rate, buffer)) {
                m_error.store(AAUDIO_ERROR_DISCONNECTED, std::memory_order_release);
                m_next_retry = now + std::chrono::seconds(2);
                return;
            }
        }
        if (!m_initialized || !m_ring || !left || !right || frames == 0) return;
        const u32 count = frames > 65536 ? 65536u : static_cast<u32>(frames);
        m_ring->push(left, right, count);
    }

private:
    bool failed_open() {
        shutdown();
        // If a resume fails transiently, submit_mix retries on the game
        // thread instead of leaving an existing device silent forever.
        m_error.store(AAUDIO_ERROR_DISCONNECTED, std::memory_order_release);
        return false;
    }

    static aaudio_data_callback_result_t data_callback(AAudioStream*, void* context,
                                                       void* data, int32_t frames) {
        auto* self = static_cast<AAudioDevice*>(context);
        if (data && frames > 0 && self->m_ring) {
            self->m_ring->pop(static_cast<f32*>(data), static_cast<u32>(frames));
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void error_callback(AAudioStream*, void* context, aaudio_result_t error) {
        auto* self = static_cast<AAudioDevice*>(context);
        self->m_error.store(error, std::memory_order_release);
    }

    AAudioStream* m_stream = nullptr;
    std::unique_ptr<AudioRingBuffer> m_ring;
    std::atomic<aaudio_result_t> m_error{AAUDIO_OK};
    std::chrono::steady_clock::time_point m_next_retry{};
    u32 m_sample_rate = kDefaultSampleRate;
    u32 m_buffer_frames = kDefaultBufferFrames;
    bool m_initialized = false; // accessed only from the game thread
};

} // namespace

std::unique_ptr<AudioDevice> create_android_output_device() {
    auto device = std::make_unique<AAudioDevice>();
    if (!device->initialize(kDefaultSampleRate, kDefaultBufferFrames)) return nullptr;
    return device;
}

} // namespace nf::audio
