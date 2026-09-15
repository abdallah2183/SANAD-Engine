// NF/Audio/src/Windows/WasapiAudioDevice.cpp — WASAPI shared-mode output.
//
// Event-driven render loop: the OS signals when the endpoint needs frames,
// we pop them from the ring (silence on underrun) and hand the buffer back.
// Every COM call is checked; any failure unwinds to a clean uninitialized
// state so initialize() can honestly return false.

#ifdef _WIN32

#include <NF/Audio/WasapiAudioDevice.hpp>

#include <NF/Core/Logger.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>

namespace nf::audio {

namespace {

constexpr u32 kRingFrames = 65536; // ~1.5 s at 44.1 kHz: absorbs combobox stalls, not songs
constexpr u32 kRefTimesPerSecond = 10000000;

void log_hresult(const char* what, HRESULT hr) {
    NF_LOG_ERROR(LogCategory::Audio, "WASAPI: {} failed (hr=0x{:08X})", what,
                 static_cast<unsigned>(hr));
}

} // namespace

AudioRingBuffer::AudioRingBuffer(u32 capacity_frames) {
    // Round up to a power of two so the mask trick works.
    u32 cap = 1;
    while (cap < capacity_frames && cap < (1u << 30)) {
        cap <<= 1;
    }
    m_capacity = cap;
    m_mask = cap - 1;
    m_data.assign(static_cast<size_t>(cap) * 2, 0.0f);
}

u32 AudioRingBuffer::push(const f32* left, const f32* right, u32 frames) {
    if (left == nullptr || right == nullptr || frames == 0) {
        return 0;
    }
    const u32 w = m_write.load(std::memory_order_relaxed);
    const u32 r = m_read.load(std::memory_order_acquire);
    const u32 free = m_capacity - (w - r);
    const u32 n = frames < free ? frames : free;
    if (n < frames) {
        m_overruns.fetch_add(static_cast<u64>(frames - n), std::memory_order_relaxed);
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 slot = (w + i) & m_mask;
        m_data[static_cast<size_t>(slot) * 2] = left[i];
        m_data[static_cast<size_t>(slot) * 2 + 1] = right[i];
    }
    m_write.store(w + n, std::memory_order_release);
    return n;
}

u32 AudioRingBuffer::pop(f32* out_interleaved, u32 frames) {
    if (out_interleaved == nullptr || frames == 0) {
        return 0;
    }
    const u32 r = m_read.load(std::memory_order_relaxed);
    const u32 w = m_write.load(std::memory_order_acquire);
    const u32 avail = w - r;
    const u32 n = frames < avail ? frames : avail;
    if (n < frames) {
        m_underruns.fetch_add(static_cast<u64>(frames - n), std::memory_order_relaxed);
    }
    // Contiguous run first; a wrapped run needs two copies (a single memcpy
    // past the vector end would over-read the heap, even when overwritten).
    const u32 base = r & m_mask;
    const u32 first = (base + n <= m_capacity) ? n : (m_capacity - base);
    std::memcpy(out_interleaved, &m_data[static_cast<size_t>(base) * 2],
                static_cast<size_t>(first) * 2 * sizeof(f32));
    if (first < n) {
        const u32 second = n - first;
        std::memcpy(out_interleaved + static_cast<size_t>(first) * 2, m_data.data(),
                    static_cast<size_t>(second) * 2 * sizeof(f32));
    }
    // Zero-fill the shortfall (silence, never stale ring data).
    for (u32 i = n; i < frames; ++i) {
        out_interleaved[static_cast<size_t>(i) * 2] = 0.0f;
        out_interleaved[static_cast<size_t>(i) * 2 + 1] = 0.0f;
    }
    m_read.store(r + n, std::memory_order_release);
    return n;
}

u32 AudioRingBuffer::stored_frames() const {
    const u32 w = m_write.load(std::memory_order_relaxed);
    const u32 r = m_read.load(std::memory_order_relaxed);
    return w - r;
}

u32 resample_linear(const f32* in_l, const f32* in_r, u32 in_frames, u32 in_rate, f32* out_l,
                    f32* out_r, u32 out_capacity, u32 out_rate, double& io_pos) {
    if (in_l == nullptr || in_r == nullptr || out_l == nullptr || out_r == nullptr ||
        in_frames == 0 || out_capacity == 0 || in_rate == 0 || out_rate == 0) {
        return 0;
    }
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    const f32 hold_l = in_l[in_frames - 1];
    const f32 hold_r = in_r[in_frames - 1];
    u32 written = 0;
    double pos = io_pos;
    while (written < out_capacity && pos < static_cast<double>(in_frames)) {
        const u32 i = static_cast<u32>(pos);
        const double frac = pos - static_cast<double>(i);
        const f32 s0l = in_l[i];
        const f32 s0r = in_r[i];
        const f32 s1l = (i + 1 < in_frames) ? in_l[i + 1] : hold_l;
        const f32 s1r = (i + 1 < in_frames) ? in_r[i + 1] : hold_r;
        const float f = static_cast<float>(frac);
        out_l[written] = s0l + (s1l - s0l) * f;
        out_r[written] = s0r + (s1r - s0r) * f;
        ++written;
        pos += step;
    }
    io_pos = pos - static_cast<double>(in_frames);
    if (io_pos < 0.0) {
        // Capacity capped the block early (caller undersized): restart at the
        // block start rather than carry a negative position (casting that to
        // u32 next call would be UB). Repeats a few samples; size the output
        // generously and this never happens.
        io_pos = 0.0;
    }
    return written;
}

bool WasapiAudioDevice::initialize(u32 sample_rate, u32 buffer_frames) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (m_initialized.load(std::memory_order_acquire)) {
        // Re-init is re-open, not a second open: tear down first. shutdown()
        // itself locks, so the unlocked core is used (same thread, same lock
        // — a std::mutex would deadlock here).
        shutdown_impl();
    }
    if (sample_rate == 0 || buffer_frames == 0) {
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        log_hresult("CoInitializeEx", hr);
        return false;
    }
    m_com_initialized = true;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        log_hresult("CoCreateInstance(MMDeviceEnumerator)", hr);
        release_com();
        return false;
    }

    IMMDevice* endpoint = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    enumerator->Release();
    if (FAILED(hr) || endpoint == nullptr) {
        // No output device (headless/CI/server): not an error, just silence.
        // Logged at info — a warning here would cry wolf on every runner.
        NF_LOG_INFO(LogCategory::Audio, "WASAPI: no default render endpoint, staying silent");
        release_com();
        return false;
    }

    IAudioClient* client = nullptr;
    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&client));
    endpoint->Release();
    if (FAILED(hr) || client == nullptr) {
        log_hresult("Activate(IAudioClient)", hr);
        release_com();
        return false;
    }
    m_client = client;

    // The endpoint's mix format decides the stream: shared-mode streams must
    // match it closely, and some drivers reject anything else at Initialize
    // time even when IsFormatSupported nodded (AUDCLNT_E_UNSUPPORTED_FORMAT
    // on exactly this machine's default endpoint). Ladder, most to least
    // preferred — anything but float stereo fails the whole open, because a
    // channel/format converter is a second feature, not a fallback:
    //   1. mix format itself, when it is float stereo (any rate — the
    //      resampler below bridges engine rate to mix rate);
    //   2. float stereo at the mix rate, negotiated explicitly.
    u32 mix_rate = 0;
    {
        WAVEFORMATEX* mix = nullptr;
        const HRESULT mix_hr = client->GetMixFormat(&mix);
        bool mix_usable = false;
        if (SUCCEEDED(mix_hr) && mix != nullptr) {
            mix_rate = mix->nSamplesPerSec;
            if (mix->nChannels == 2 && mix_rate != 0) {
                if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32) {
                    mix_usable = true;
                } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
                    const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
                    mix_usable = (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
                                 (ex->Samples.wValidBitsPerSample == 32);
                }
            }
        }
        if (mix != nullptr) {
            CoTaskMemFree(mix);
        }
        if (!mix_usable || mix_rate == 0) {
            NF_LOG_ERROR(LogCategory::Audio, "WASAPI: mix format is not float stereo; no converter");
            release_com();
            return false;
        }
    }

    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = mix_rate;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 2 * 4;
    wave.Format.nAvgBytesPerSec = mix_rate * 2 * 4;
    wave.Format.cbSize = 22;
    wave.Samples.wValidBitsPerSample = 32;
    wave.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    WAVEFORMATEX* closest = nullptr;
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                   reinterpret_cast<WAVEFORMATEX*>(&wave), &closest);
    if (closest != nullptr) {
        CoTaskMemFree(closest);
    }
    if (FAILED(hr)) {
        log_hresult("IsFormatSupported(float stereo at mix rate)", hr);
        release_com();
        return false;
    }

    const REFERENCE_TIME hns = static_cast<REFERENCE_TIME>(buffer_frames) * kRefTimesPerSecond /
                               static_cast<REFERENCE_TIME>(sample_rate);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hns, 0,
                            reinterpret_cast<WAVEFORMATEX*>(&wave), nullptr);
    if (FAILED(hr)) {
        log_hresult("IAudioClient::Initialize", hr);
        release_com();
        return false;
    }

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        NF_LOG_ERROR(LogCategory::Audio, "WASAPI: CreateEvent failed");
        release_com();
        return false;
    }
    m_event = event;
    hr = client->SetEventHandle(event);
    if (FAILED(hr)) {
        log_hresult("SetEventHandle", hr);
        release_com();
        return false;
    }

    UINT32 endpoint_frames = 0;
    hr = client->GetBufferSize(&endpoint_frames);
    if (FAILED(hr) || endpoint_frames == 0) {
        log_hresult("GetBufferSize", hr);
        release_com();
        return false;
    }
    m_endpoint_buffer_frames = endpoint_frames;

    IAudioRenderClient* render = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (FAILED(hr) || render == nullptr) {
        log_hresult("GetService(IAudioRenderClient)", hr);
        release_com();
        return false;
    }
    m_render_client = render;

    m_sample_rate = sample_rate;
    m_mix_rate = mix_rate;
    m_resample_pos = 0.0;
    // The member ring already carries kRingFrames capacity from construction
    // (atomics are neither copyable nor movable, so it cannot be reassigned).
    m_stop.store(false, std::memory_order_release);
    m_submitted.store(0, std::memory_order_relaxed);
    m_rendered.store(0, std::memory_order_relaxed);

    hr = client->Start();
    if (FAILED(hr)) {
        log_hresult("IAudioClient::Start", hr);
        release_com();
        return false;
    }

    try {
        m_thread = std::thread(&WasapiAudioDevice::render_loop, this);
    } catch (...) {
        NF_LOG_ERROR(LogCategory::Audio, "WASAPI: render thread failed to start");
        client->Stop();
        release_com();
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    if (m_mix_rate == sample_rate) {
        NF_LOG_INFO(LogCategory::Audio, "WASAPI: shared float-stereo @ {} Hz (endpoint {} frames)",
                    sample_rate, endpoint_frames);
    } else {
        NF_LOG_INFO(LogCategory::Audio,
                    "WASAPI: shared float-stereo @ {} Hz endpoint, resampling {} Hz mix (endpoint {} frames)",
                    mix_rate, sample_rate, endpoint_frames);
    }
    return true;
}

void WasapiAudioDevice::render_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* client = static_cast<IAudioClient*>(m_client);
    auto* render = static_cast<IAudioRenderClient*>(m_render_client);
    auto event = static_cast<HANDLE>(m_event);

    std::vector<f32> scratch;
    scratch.reserve(4096 * 2);

    while (!m_stop.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForSingleObject(event, 2000);
        if (m_stop.load(std::memory_order_acquire)) {
            break;
        }
        if (wait != WAIT_OBJECT_0) {
            continue; // timeout with no data needed; loop back and re-check stop
        }
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            continue;
        }
        u32 want = (m_endpoint_buffer_frames > padding) ? (m_endpoint_buffer_frames - padding) : 0;
        if (want == 0) {
            continue;
        }
        if (scratch.size() < static_cast<size_t>(want) * 2) {
            scratch.resize(static_cast<size_t>(want) * 2);
        }
        m_ring.pop(scratch.data(), want);

        BYTE* dst = nullptr;
        if (client != nullptr && render != nullptr && SUCCEEDED(render->GetBuffer(want, &dst)) &&
            dst != nullptr) {
            std::memcpy(dst, scratch.data(), static_cast<size_t>(want) * 2 * sizeof(f32));
            render->ReleaseBuffer(want, 0);
            m_rendered.fetch_add(static_cast<u64>(want), std::memory_order_relaxed);
        }
    }
    CoUninitialize();
}

void WasapiAudioDevice::shutdown() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    shutdown_impl();
}

// Unlocked core: callers hold m_state_mutex (initialize re-entry and public
// shutdown converge here instead of locking twice).
void WasapiAudioDevice::shutdown_impl() {
    m_stop.store(true, std::memory_order_release);
    if (m_event != nullptr) {
        SetEvent(static_cast<HANDLE>(m_event));
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    release_com();
    m_initialized.store(false, std::memory_order_release);
}

void WasapiAudioDevice::release_com() noexcept {
    // Client first (it owns the stream), then the render client, then the
    // event. Order is teardown order, not importance.
    if (m_client != nullptr) {
        static_cast<IAudioClient*>(m_client)->Stop();
        static_cast<IAudioClient*>(m_client)->Release();
        m_client = nullptr;
    }
    if (m_render_client != nullptr) {
        static_cast<IAudioRenderClient*>(m_render_client)->Release();
        m_render_client = nullptr;
    }
    if (m_event != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_event));
        m_event = nullptr;
    }
    m_endpoint_buffer_frames = 0;
    if (m_com_initialized) {
        CoUninitialize();
        m_com_initialized = false;
    }
}

void WasapiAudioDevice::request_buffer(f32* left, f32* right, usize num_frames) {
    // The device contributes no base of its own; the scene mix arrives via
    // submit_mix. Fill silence so a caller that skips the submit still mixes
    // over a defined buffer.
    if (left == nullptr || right == nullptr) {
        return;
    }
    for (usize i = 0; i < num_frames; ++i) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
}

void WasapiAudioDevice::submit_mix(const f32* left, const f32* right, usize num_frames) {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return;
    }
    if (left == nullptr || right == nullptr || num_frames == 0) {
        return;
    }
    // Clamp absurd blocks: a caller bug must clip audio, not grow memory.
    u32 frames = num_frames > 65536 ? 65536 : static_cast<u32>(num_frames);
    u32 stored = 0;
    if (m_mix_rate == 0 || m_mix_rate == m_sample_rate) {
        stored = m_ring.push(left, right, frames);
    } else {
        // Endpoint runs at its own rate: convert into scratch, then push.
        // Single-producer only (the audio step thread) — the scratch reuse
        // and the resample position both assume it.
        const double ratio = static_cast<double>(m_mix_rate) / static_cast<double>(m_sample_rate);
        const u32 need =
            static_cast<u32>(static_cast<double>(frames) * ratio) + 2u;
        if (m_resample_scratch_l.size() < need) {
            m_resample_scratch_l.resize(need);
            m_resample_scratch_r.resize(need);
        }
        const u32 out_frames =
            resample_linear(left, right, frames, m_sample_rate, m_resample_scratch_l.data(),
                            m_resample_scratch_r.data(), need, m_mix_rate, m_resample_pos);
        stored = m_ring.push(m_resample_scratch_l.data(), m_resample_scratch_r.data(), out_frames);
    }
    m_submitted.fetch_add(static_cast<u64>(stored), std::memory_order_relaxed);
}

} // namespace nf::audio

#endif // _WIN32
