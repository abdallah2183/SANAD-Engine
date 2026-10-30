#include <NF/Gameplay/Sequencer.hpp>
#include <NF/Test/TestFramework.hpp>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(float_track_linear_interpolation) {
    FloatTrack track("fov");
    track.add_key(0.0f, 60.0f);
    track.add_key(2.0f, 90.0f);

    NF_CHECK_NEAR(track.evaluate(-1.0f), 60.0f, 1e-4f);
    NF_CHECK_NEAR(track.evaluate(0.0f), 60.0f, 1e-4f);
    NF_CHECK_NEAR(track.evaluate(1.0f), 75.0f, 1e-4f);
    NF_CHECK_NEAR(track.evaluate(2.0f), 90.0f, 1e-4f);
    NF_CHECK_NEAR(track.evaluate(3.0f), 90.0f, 1e-4f);
}

NF_TEST(float_track_step_and_hermite_interpolation) {
    FloatTrack step_track("step");
    step_track.add_key(0.0f, 10.0f, InterpolationMode::Step);
    step_track.add_key(2.0f, 20.0f, InterpolationMode::Step);

    NF_CHECK_NEAR(step_track.evaluate(0.5f), 10.0f, 1e-4f);
    NF_CHECK_NEAR(step_track.evaluate(1.99f), 10.0f, 1e-4f);
    NF_CHECK_NEAR(step_track.evaluate(2.0f), 20.0f, 1e-4f);

    FloatTrack hermite_track("smooth");
    hermite_track.add_key(0.0f, 0.0f, InterpolationMode::CubicHermite);
    hermite_track.add_key(1.0f, 10.0f, InterpolationMode::CubicHermite);

    // At t=0.5, smoothstep is 0.5, value = 5.0
    NF_CHECK_NEAR(hermite_track.evaluate(0.5f), 5.0f, 1e-4f);
    // Smoothstep has zero tangent at start: at t=0.1, smoothstep(0.1)=0.028, value=0.28 (less than linear 1.0)
    NF_CHECK(hermite_track.evaluate(0.1f) < 0.5f);
}

NF_TEST(vector_track_translation_evaluation) {
    VectorTrack track("cam_pos");
    track.add_key(0.0f, Vec3{0, 2, -10});
    track.add_key(4.0f, Vec3{10, 5, 0});

    const Vec3 mid = track.evaluate(2.0f);
    NF_CHECK_NEAR(mid.x, 5.0f, 1e-4f);
    NF_CHECK_NEAR(mid.y, 3.5f, 1e-4f);
    NF_CHECK_NEAR(mid.z, -5.0f, 1e-4f);
}

NF_TEST(event_track_collection_window) {
    EventTrack track;
    track.add_event(1.0f, "fade_in");
    track.add_event(2.5f, "sfx_thunder", "vol=0.8");
    track.add_event(4.0f, "spawn_boss", "id=boss_dragon");

    std::vector<EventTrack::EventKey> triggered;
    track.collect_events(0.0f, 1.5f, triggered);
    NF_CHECK_EQ(triggered.size(), 1u);
    NF_CHECK_EQ(triggered[0].name, "fade_in");

    triggered.clear();
    track.collect_events(1.5f, 3.0f, triggered);
    NF_CHECK_EQ(triggered.size(), 1u);
    NF_CHECK_EQ(triggered[0].name, "sfx_thunder");
    NF_CHECK_EQ(triggered[0].payload, "vol=0.8");

    triggered.clear();
    track.collect_events(0.0f, 5.0f, triggered);
    NF_CHECK_EQ(triggered.size(), 3u);
}

NF_TEST(cutscene_sequence_playback_lifecycle) {
    CutsceneSequence seq("IntroCutscene", 10.0f);
    NF_CHECK_EQ(seq.state(), PlaybackState::Stopped);
    NF_CHECK_NEAR(seq.current_time(), 0.0f, 1e-4f);

    seq.play();
    NF_CHECK_EQ(seq.state(), PlaybackState::Playing);

    seq.step(1.5f);
    NF_CHECK_NEAR(seq.current_time(), 1.5f, 1e-4f);

    seq.pause();
    NF_CHECK_EQ(seq.state(), PlaybackState::Paused);
    seq.step(1.0f); // Paused: should not advance
    NF_CHECK_NEAR(seq.current_time(), 1.5f, 1e-4f);

    seq.stop();
    NF_CHECK_EQ(seq.state(), PlaybackState::Stopped);
    NF_CHECK_NEAR(seq.current_time(), 0.0f, 1e-4f);
}

NF_TEST(cutscene_sequence_scrub_and_clamp) {
    CutsceneSequence seq("Timeline", 5.0f);
    seq.scrub_to(3.2f);
    NF_CHECK_NEAR(seq.current_time(), 3.2f, 1e-4f);

    seq.scrub_to(-2.0f);
    NF_CHECK_NEAR(seq.current_time(), 0.0f, 1e-4f);

    seq.scrub_to(10.0f);
    NF_CHECK_NEAR(seq.current_time(), 5.0f, 1e-4f);
}

NF_TEST(cutscene_sequence_loop_mode_wrap) {
    CutsceneSequence seq("LoopingSequence", 4.0f);
    seq.set_loop_mode(LoopMode::Loop);
    seq.play();

    seq.step(3.0f);
    NF_CHECK_NEAR(seq.current_time(), 3.0f, 1e-4f);

    seq.step(2.0f); // 3 + 2 = 5 -> wrapped in 4s -> 1.0s
    NF_CHECK_NEAR(seq.current_time(), 1.0f, 1e-4f);
    NF_CHECK_EQ(seq.state(), PlaybackState::Playing);
}

NF_TEST(cutscene_sequence_ping_pong_playback) {
    CutsceneSequence seq("PingPongSequence", 3.0f);
    seq.set_loop_mode(LoopMode::PingPong);
    seq.play();

    seq.step(2.0f);
    NF_CHECK_NEAR(seq.current_time(), 2.0f, 1e-4f);

    seq.step(1.5f); // Reaches 3.0 and bounces backward by 0.5 -> 2.5s
    NF_CHECK_NEAR(seq.current_time(), 2.5f, 1e-4f);

    seq.step(1.0f); // Continues backward -> 1.5s
    NF_CHECK_NEAR(seq.current_time(), 1.5f, 1e-4f);
}

NF_TEST(cutscene_sequence_speed_scaling) {
    CutsceneSequence seq("FastForward", 10.0f);
    seq.set_playback_speed(2.0f); // 2x speed
    seq.play();

    seq.step(1.0f);
    NF_CHECK_NEAR(seq.current_time(), 2.0f, 1e-4f);

    seq.set_playback_speed(0.5f); // 0.5x slow-motion
    seq.step(2.0f);
    NF_CHECK_NEAR(seq.current_time(), 3.0f, 1e-4f);
}

NF_TEST(cutscene_sequence_multi_track_evaluation) {
    CutsceneSequence seq("FullCinematic", 6.0f);

    auto& fov = seq.add_float_track("camera_fov");
    fov.add_key(0.0f, 60.0f);
    fov.add_key(6.0f, 40.0f);

    auto& pos = seq.add_vector_track("camera_pos");
    pos.add_key(0.0f, Vec3{0, 2, 0});
    pos.add_key(6.0f, Vec3{0, 2, 10});

    seq.event_track().add_event(3.0f, "climax_sound");

    seq.play();
    std::vector<EventTrack::EventKey> events;
    seq.step(3.0f, &events);

    NF_CHECK_NEAR(seq.evaluate_float("camera_fov"), 50.0f, 1e-4f);
    const Vec3 p = seq.evaluate_vector("camera_pos");
    NF_CHECK_NEAR(p.z, 5.0f, 1e-4f);

    NF_CHECK_EQ(events.size(), 1u);
    NF_CHECK_EQ(events[0].name, "climax_sound");
}
