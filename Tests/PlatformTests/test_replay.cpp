// PlatformTests — input recording & playback (design doc 114/115).
//
// No window, no OS messages: synthetic InputSystem events stand in for the
// platform layer, which is exactly the seam playback re-injects through.

#include <NF/Test/TestFramework.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Platform/Replay.hpp>

#include <string>
#include <vector>

using namespace nf;

namespace {

// Drives one synthetic frame: applies OS-side events, captures the tick.
void synth_frame(InputSystem& input, InputRecorder& rec) {
    rec.capture_tick(input.state());
    input.begin_frame(); // next frame starts clean, like the game loop
}

} // namespace

NF_TEST(replay_records_key_press_and_release) {
    InputSystem& input = InputSystem::instance();
    input.begin_frame();
    InputRecorder rec;

    input.on_key_down(KeyCode::A);
    synth_frame(input, rec); // tick 0: A down
    synth_frame(input, rec); // tick 1: silence
    input.on_key_up(KeyCode::A);
    synth_frame(input, rec); // tick 2: A up

    NF_CHECK(rec.tick() == 3);
    NF_CHECK(rec.events().size() == 2);
    NF_CHECK(rec.events()[0].tick == 0);
    NF_CHECK(rec.events()[0].type == ReplayEventType::KeyDown);
    NF_CHECK(rec.events()[0].code == static_cast<u32>(KeyCode::A));
    NF_CHECK(rec.events()[1].tick == 2);
    NF_CHECK(rec.events()[1].type == ReplayEventType::KeyUp);
}

NF_TEST(replay_playback_reproduces_key_state) {
    InputSystem& input = InputSystem::instance();
    input.begin_frame();
    InputRecorder rec;
    input.on_key_down(KeyCode::Space);
    synth_frame(input, rec);
    input.on_key_up(KeyCode::Space);
    input.on_key_down(KeyCode::W);
    synth_frame(input, rec);
    synth_frame(input, rec);

    InputPlayback play(rec.events());
    // Span covers the last RECORDED tick (trailing silence lives in the
    // recorder's tick count, preserved by save/load — see below).
    NF_CHECK(play.tick_count() == 2);

    input.begin_frame();
    play.apply_tick(input, 0);
    NF_CHECK(input.is_key_down(KeyCode::Space));
    NF_CHECK(input.is_key_pressed(KeyCode::Space));
    input.begin_frame();
    play.apply_tick(input, 1);
    NF_CHECK(!input.is_key_down(KeyCode::Space));
    NF_CHECK(input.is_key_down(KeyCode::W));
    input.begin_frame();
    play.apply_tick(input, 2);
    NF_CHECK(input.is_key_down(KeyCode::W)); // held across the silent tick
    NF_CHECK(!input.is_key_pressed(KeyCode::W)); // ...but not re-pressed
}

NF_TEST(replay_records_mouse_buttons_moves_and_wheel) {
    InputSystem& input = InputSystem::instance();
    input.begin_frame();
    InputRecorder rec;
    input.on_mouse_button(MouseButton::Left, true);
    input.on_mouse_move(100.0f, 200.0f);
    input.on_mouse_scroll(1.0f);
    synth_frame(input, rec);
    input.on_mouse_button(MouseButton::Left, false);
    synth_frame(input, rec);

    InputPlayback play(rec.events());
    input.begin_frame();
    play.apply_tick(input, 0);
    NF_CHECK(input.is_mouse_down(MouseButton::Left));
    NF_CHECK_NEAR(input.mouse_x(), 100.0f, 1e-5f);
    NF_CHECK_NEAR(input.mouse_y(), 200.0f, 1e-5f);
    NF_CHECK_NEAR(input.scroll_delta(), 1.0f, 1e-6f);
    input.begin_frame();
    play.apply_tick(input, 1);
    NF_CHECK(!input.is_mouse_down(MouseButton::Left));
}

NF_TEST(replay_save_load_roundtrip) {
    InputSystem& input = InputSystem::instance();
    input.begin_frame();
    InputRecorder rec;
    input.on_key_down(KeyCode::S);
    input.on_mouse_move(7.0f, 9.0f);
    synth_frame(input, rec);
    synth_frame(input, rec);

    const std::vector<u8> bytes = rec.save();
    NF_CHECK(!bytes.empty());

    InputRecorder restored;
    std::string err;
    NF_CHECK(restored.load(bytes.data(), bytes.size(), err));
    NF_CHECK(err.empty());
    NF_CHECK(restored.events() == rec.events());
    NF_CHECK(restored.tick() == rec.tick());

    // Playback from the restored copy behaves identically.
    InputPlayback play(restored.events());
    input.begin_frame();
    play.apply_tick(input, 0);
    NF_CHECK(input.is_key_down(KeyCode::S));
    NF_CHECK_NEAR(input.mouse_x(), 7.0f, 1e-5f);
}

NF_TEST(replay_rejects_corrupt_streams) {
    InputRecorder rec;
    std::string err;
    NF_CHECK(!rec.load(nullptr, 0, err));
    const u8 short_buf[] = {'N', 'F', 'R', 'P', 1, 0, 0, 0};
    NF_CHECK(!rec.load(short_buf, sizeof(short_buf), err));
    const u8 bad_magic[] = {'X', 'X', 'X', 'X', 1, 0, 0, 0, 0, 0, 0, 0};
    NF_CHECK(!rec.load(bad_magic, sizeof(bad_magic), err));
    NF_CHECK(!err.empty());
    NF_CHECK(rec.events().empty());
}

NF_TEST(replay_empty_stream_is_valid) {
    InputRecorder rec;
    const std::vector<u8> bytes = rec.save();
    InputRecorder restored;
    std::string err;
    NF_CHECK(restored.load(bytes.data(), bytes.size(), err));
    NF_CHECK(restored.events().empty());
    InputPlayback play(restored.events());
    NF_CHECK(play.tick_count() == 0);
    InputSystem& input = InputSystem::instance();
    input.begin_frame();
    play.apply_tick(input, 0); // silence replays as silence, no crash
    play.reset();
    play.apply_tick(input, 5); // seeking past the end is a no-op
}
