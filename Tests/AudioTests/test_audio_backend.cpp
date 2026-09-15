// Tests/AudioTests/test_audio_backend.cpp — output backends (WASAPI + factory).
//
// The mixer math is covered in test_audio.cpp without any hardware. These
// tests cover the seam to the OS: the SPSC ring both backends' correctness
// rests on, the factory that never returns null, and the WASAPI device
// itself — which must degrade to silence (not crash, not hang, not fail the
// suite) on machines with no audio endpoint, and must actually move frames
// on machines with one.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioEngine.hpp>
#ifdef _WIN32
#include <NF/Audio/WasapiAudioDevice.hpp>
#endif

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace nf;

#ifdef _WIN32
using namespace nf::audio;

NF_TEST(audio_ring_round_trips_interleaved) {
    AudioRingBuffer ring(8);
    const f32 left[] = {1.0f, 2.0f, 3.0f};
    const f32 right[] = {4.0f, 5.0f, 6.0f};
    NF_CHECK_EQ(ring.push(left, right, 3), 3u);
    NF_CHECK_EQ(ring.stored_frames(), 3u);
    f32 out[6] = {};
    NF_CHECK_EQ(ring.pop(out, 3), 3u);
    NF_CHECK_EQ(ring.stored_frames(), 0u);
    // Interleaved LRLR.
    NF_CHECK_NEAR(out[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(out[1], 4.0f, 1e-6f);
    NF_CHECK_NEAR(out[2], 2.0f, 1e-6f);
    NF_CHECK_NEAR(out[3], 5.0f, 1e-6f);
    NF_CHECK_NEAR(out[4], 3.0f, 1e-6f);
    NF_CHECK_NEAR(out[5], 6.0f, 1e-6f);
}

NF_TEST(audio_ring_wraps_and_counts_over_underruns) {
    AudioRingBuffer ring(4);
    const f32 l[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const f32 r[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    NF_CHECK_EQ(ring.push(l, r, 4), 4u);
    // Full: the excess is dropped (latency stays bounded), counted, and the
    // stored frames are untouched.
    NF_CHECK_EQ(ring.push(l, r, 2), 0u);
    NF_CHECK_EQ(ring.overruns(), 2u);
    NF_CHECK_EQ(ring.stored_frames(), 4u);

    // Drain 3 (wrapping the read cursor), push 3 more (wrapping the write
    // cursor), then drain all 4: order must survive both wraps.
    f32 out[8] = {};
    NF_CHECK_EQ(ring.pop(out, 3), 3u);
    const f32 l2[3] = {11.0f, 12.0f, 13.0f};
    const f32 r2[3] = {14.0f, 15.0f, 16.0f};
    NF_CHECK_EQ(ring.push(l2, r2, 3), 3u);
    f32 out2[8] = {};
    NF_CHECK_EQ(ring.pop(out2, 4), 4u);
    NF_CHECK_NEAR(out2[0], 4.0f, 1e-6f); // leftover frame 4
    NF_CHECK_NEAR(out2[1], 8.0f, 1e-6f);
    NF_CHECK_NEAR(out2[2], 11.0f, 1e-6f);
    NF_CHECK_NEAR(out2[3], 14.0f, 1e-6f);
    NF_CHECK_NEAR(out2[4], 12.0f, 1e-6f);
    NF_CHECK_NEAR(out2[5], 15.0f, 1e-6f);
    NF_CHECK_NEAR(out2[6], 13.0f, 1e-6f);
    NF_CHECK_NEAR(out2[7], 16.0f, 1e-6f);

    // Short read: what is there plus silence for the rest, counted.
    f32 out3[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    NF_CHECK_EQ(ring.pop(out3, 2), 0u);
    NF_CHECK_EQ(ring.underruns(), 2u);
    NF_CHECK_NEAR(out3[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(out3[3], 0.0f, 1e-6f);
}

NF_TEST(audio_ring_rejects_null_and_empty) {
    AudioRingBuffer ring(4);
    f32 v[2] = {1.0f, 2.0f};
    NF_CHECK_EQ(ring.push(nullptr, v, 1), 0u);
    NF_CHECK_EQ(ring.push(v, nullptr, 1), 0u);
    NF_CHECK_EQ(ring.push(v, v, 0), 0u);
    NF_CHECK_EQ(ring.pop(nullptr, 1), 0u);
    NF_CHECK_EQ(ring.pop(v, 0), 0u);
    NF_CHECK_EQ(ring.stored_frames(), 0u);
}

NF_TEST(audio_resample_preserves_dc_and_count) {
    // 0.1 s of DC at engine rate: linear interpolation of a constant is the
    // constant (up to fp rounding). The exact output count is deliberately
    // NOT pinned: 4410 input frames map to exactly 4800.0 outputs, which is
    // not representable in binary, so the boundary wobbles by a frame either
    // way depending on rounding. Duration is what matters (±1 frame).
    std::vector<f32> in(4410, 0.25f);
    std::vector<f32> out_l(5005, 0.0f), out_r(5005, 0.0f);
    double pos = 0.0;
    const u32 n =
        resample_linear(in.data(), in.data(), 4410, 44100, out_l.data(), out_r.data(), 5005, 48000, pos);
    NF_CHECK(n >= 4799u && n <= 4801u);
    for (u32 i = 0; i < n; ++i) {
        NF_CHECK_NEAR(out_l[i], 0.25f, 1e-6f);
        NF_CHECK_NEAR(out_r[i], 0.25f, 1e-6f);
    }
    // Untouched tail stays as constructed.
    NF_CHECK_NEAR(out_l[5004], 0.0f, 1e-6f);
}

NF_TEST(audio_resample_tracks_a_sine_and_chains_cleanly) {
    // 440 Hz sine at 44100: spot-check against the analytic value (linear
    // interpolation error at this ratio is far below the tolerance), then
    // prove split calls chain without clicks: two halves equal one call.
    constexpr double kPi = 3.14159265358979323846;
    std::vector<f32> in(4410);
    for (u32 i = 0; i < 4410; ++i) {
        in[i] = static_cast<f32>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / 44100.0));
    }
    std::vector<f32> whole_l(5005, 0.0f), whole_r(5005, 0.0f);
    double pos = 0.0;
    const u32 n =
        resample_linear(in.data(), in.data(), 4410, 44100, whole_l.data(), whole_r.data(), 5005, 48000, pos);
    NF_CHECK(n >= 4799u && n <= 4801u);
    // Output frame 100 sits at input time 100 * 44100/48000 / 44100 s.
    const double t100 = 100.0 * (44100.0 / 48000.0) / 44100.0;
    NF_CHECK_NEAR(whole_l[100], static_cast<f32>(std::sin(2.0 * kPi * 440.0 * t100)), 1e-2f);
    NF_CHECK_NEAR(whole_l[0], 0.0f, 1e-2f);

    std::vector<f32> half_l(5005, 0.0f), half_r(5005, 0.0f);
    double pos2 = 0.0;
    const u32 n1 =
        resample_linear(in.data(), in.data(), 2205, 44100, half_l.data(), half_r.data(), 5005, 48000, pos2);
    const u32 n2 = resample_linear(in.data() + 2205, in.data() + 2205, 2205, 44100,
                                   half_l.data() + n1, half_r.data() + n1, 5005 - n1, 48000, pos2);
    // Split/whole counts agree up to the inevitable float boundary wobble:
    // (n1*s - 2205) + m*s and (n1+m)*s round differently, so a position
    // within an ulp of the block edge lands on either side. Content below
    // uses a tolerance that survives the same effect one level deeper: near
    // an integer position the two paths can index adjacent input samples
    // (up to one sample-step apart, ~0.06 here), so exactness is unassertable
    // and only divergence is. Accuracy itself is pinned above (analytic sine)
    // and in the DC test; this loop pins chaining (no clicks, no garbage).
    NF_CHECK(n1 + n2 + 1 >= n && n1 + n2 <= n + 1);
    const u32 cmp = n1 + n2 < n ? n1 + n2 : n;
    for (u32 i = 0; i < cmp; ++i) {
        NF_CHECK_NEAR(half_l[i], whole_l[i], 7e-2f);
    }
}

NF_TEST(audio_resample_rejects_degenerate_input) {
    f32 v[2] = {1.0f, 2.0f};
    double pos = 0.0;
    NF_CHECK_EQ(resample_linear(nullptr, v, 2, 44100, v, v, 2, 48000, pos), 0u);
    NF_CHECK_EQ(resample_linear(v, v, 0, 44100, v, v, 2, 48000, pos), 0u);
    NF_CHECK_EQ(resample_linear(v, v, 2, 0, v, v, 2, 48000, pos), 0u);
    NF_CHECK_EQ(resample_linear(v, v, 2, 44100, v, v, 0, 48000, pos), 0u);
}

NF_TEST(wasapi_init_is_safe_without_hardware) {
    // Either outcome is a pass: an endpoint gives a live device, no endpoint
    // gives a clean false (never a crash, a hang, or a half-open device).
    // Headless CI runners take the second branch; dev machines the first.
    WasapiAudioDevice dev;
    NF_CHECK(!dev.is_initialized());
    const bool ok = dev.initialize(44100, 1024);
    if (ok) {
        NF_CHECK(dev.is_initialized());
        NF_CHECK_EQ(dev.sample_rate(), 44100u);
        NF_CHECK(std::string(dev.backend_name()) == "wasapi-shared");
        NF_CHECK(dev.accepts_push());
        // Silence in, silence mixed: request_buffer is defined even for a
        // push device (callers that skip the submit still mix over zeros).
        f32 l[8], r[8];
        for (int i = 0; i < 8; ++i) {
            l[i] = 1.0f;
            r[i] = 1.0f;
        }
        dev.request_buffer(l, r, 8);
        NF_CHECK_NEAR(l[0], 0.0f, 1e-6f);
        NF_CHECK_NEAR(r[7], 0.0f, 1e-6f);

        // Push 0.1 s of tone; the render thread must consume it (or at least
        // accept it) within half a second on a live endpoint. The submitted
        // count is in MIX-rate frames (resampled when the endpoint disagrees
        // with the engine rate), so it is checked against the ratio, not the
        // input count.
        std::vector<f32> tone(4410, 0.25f);
        dev.submit_mix(tone.data(), tone.data(), 4410);
        const u64 expect =
            static_cast<u64>(4410.0 * static_cast<double>(dev.mix_rate()) / 44100.0);
        const u64 got = dev.frames_submitted();
        NF_CHECK(got + 1 >= expect && got <= expect + 1);
        bool consumed = false;
        for (int i = 0; i < 50; ++i) {
            if (dev.frames_rendered() > 0) {
                consumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        NF_CHECK(consumed);
        dev.shutdown();
        NF_CHECK(!dev.is_initialized());
        dev.shutdown(); // idempotent
    } else {
        NF_CHECK(!dev.is_initialized());
    }
}
#endif // _WIN32

NF_TEST(audio_factory_never_returns_null) {
    // The factory is the only sanctioned way to get a device: real hardware
    // where a backend opens, silence otherwise. Either way the runtime gets
    // an initialized device it can mix into.
    std::unique_ptr<audio::AudioDevice> dev = audio::create_output_device();
    NF_CHECK(dev != nullptr);
    NF_CHECK(dev->is_initialized());
    NF_CHECK(dev->sample_rate() > 0);
    const std::string name(dev->backend_name());
    NF_CHECK(name == "null" || name == "wasapi-shared");
    if (!dev->accepts_push()) {
        // Null contract: defined silence, so the mix below means something.
        const usize n = 8;
        f32 left[n], right[n];
        for (usize i = 0; i < n; ++i) {
            left[i] = 2.0f;
            right[i] = 2.0f;
        }
        dev->request_buffer(left, right, n);
        NF_CHECK_NEAR(left[0], 0.0f, 1e-5f);
        NF_CHECK_NEAR(right[n - 1], 0.0f, 1e-5f);
    }
    dev->shutdown();
}
