#include <NF/Test/TestFramework.hpp>
#include <NF/Scene2D/SpriteAnimation.hpp>

using namespace nf;
using namespace nf::scene2d;

NF_TEST(sprite_clip_creation_and_duration) {
    SpriteAnimationClip clip("walk", SpriteLoopMode::Loop);
    NF_CHECK_EQ(clip.name(), "walk");
    NF_CHECK(clip.loop_mode() == SpriteLoopMode::Loop);
    NF_CHECK_EQ(clip.frame_count(), 0ULL);
    NF_CHECK_NEAR(clip.total_duration(), 0.0f, 1e-4f);

    clip.add_frame("walk_0", 0.1f, Vec2{0.0f, 0.0f}, "step_left");
    clip.add_frame("walk_1", 0.15f);
    clip.add_frame("walk_2", 0.1f, Vec2{0.0f, 0.0f}, "step_right");

    NF_CHECK_EQ(clip.frame_count(), 3ULL);
    NF_CHECK_NEAR(clip.total_duration(), 0.35f, 1e-4f);

    const auto* f0 = clip.frame(0);
    NF_CHECK(f0 != nullptr);
    NF_CHECK_EQ(f0->region_name, "walk_0");
    NF_CHECK_EQ(f0->notify_event, "step_left");
}

NF_TEST(sprite_clip_add_frames_helper) {
    SpriteAnimationClip clip("idle");
    std::vector<std::string> names = {"idle_0", "idle_1", "idle_2", "idle_3"};
    clip.add_frames(names, 0.2f);

    NF_CHECK_EQ(clip.frame_count(), 4ULL);
    NF_CHECK_NEAR(clip.total_duration(), 0.8f, 1e-4f);
}

NF_TEST(sprite_animator_play_once) {
    SpriteAnimationClip clip("jump", SpriteLoopMode::Once);
    clip.add_frame("jump_up", 0.1f);
    clip.add_frame("jump_apex", 0.1f);
    clip.add_frame("jump_fall", 0.1f);

    SpriteAnimator animator;
    animator.play(&clip);

    NF_CHECK(animator.current_state().is_playing);
    NF_CHECK(!animator.current_state().is_finished);
    NF_CHECK_EQ(animator.current_state().region_name, "jump_up");

    // Advance 0.15s -> apex
    auto st = animator.update(0.15f);
    NF_CHECK_EQ(st.current_frame_index, 1ULL);
    NF_CHECK_EQ(st.region_name, "jump_apex");

    // Advance another 0.2s -> past 0.3s end
    st = animator.update(0.2f);
    NF_CHECK_EQ(st.current_frame_index, 2ULL);
    NF_CHECK_EQ(st.region_name, "jump_fall");
    NF_CHECK(st.is_finished);
    NF_CHECK(!st.is_playing);
}

NF_TEST(sprite_animator_loop_wraps) {
    SpriteAnimationClip clip("run", SpriteLoopMode::Loop);
    clip.add_frame("run_0", 0.1f);
    clip.add_frame("run_1", 0.1f);

    SpriteAnimator animator;
    animator.play(&clip);

    // Frame 0 at t = 0
    NF_CHECK_EQ(animator.current_state().current_frame_index, 0ULL);

    // Advance 0.12s -> frame 1
    auto st = animator.update(0.12f);
    NF_CHECK_EQ(st.current_frame_index, 1ULL);

    // Advance another 0.1s (total 0.22s, wraps around 0.2s duration) -> frame 0 again
    st = animator.update(0.1f);
    NF_CHECK_EQ(st.current_frame_index, 0ULL);
    NF_CHECK(!st.is_finished);
}

NF_TEST(sprite_animator_ping_pong) {
    SpriteAnimationClip clip("breathe", SpriteLoopMode::PingPong);
    clip.add_frame("b_0", 0.1f);
    clip.add_frame("b_1", 0.1f);
    clip.add_frame("b_2", 0.1f);

    SpriteAnimator animator;
    animator.play(&clip);

    // 0.05s -> frame 0
    auto st = animator.update(0.05f);
    NF_CHECK_EQ(st.current_frame_index, 0ULL);

    // 0.25s total -> frame 2 (apex)
    st = animator.update(0.2f);
    NF_CHECK_EQ(st.current_frame_index, 2ULL);

    // Advance into reverse: total 0.35s -> bounces back to 0.25s in reverse -> frame 2/1
    st = animator.update(0.1f);
    NF_CHECK(st.current_frame_index <= 2ULL);
}

NF_TEST(sprite_animator_speed_scaling) {
    SpriteAnimationClip clip("fast_run", SpriteLoopMode::Once);
    clip.add_frame("f0", 0.5f);
    clip.add_frame("f1", 0.5f);

    SpriteAnimator animator;
    animator.set_speed(2.0f); // 2x speed
    animator.play(&clip);

    // In 0.3s of dt, elapsed time advances 0.6s -> reaches frame 1!
    auto st = animator.update(0.3f);
    NF_CHECK_EQ(st.current_frame_index, 1ULL);
}

NF_TEST(sprite_animator_pause_and_resume) {
    SpriteAnimationClip clip("spin", SpriteLoopMode::Loop);
    clip.add_frame("s0", 0.1f);
    clip.add_frame("s1", 0.1f);

    SpriteAnimator animator;
    animator.play(&clip);
    animator.pause();
    NF_CHECK(animator.is_paused());

    // Advance time while paused
    auto st = animator.update(0.5f);
    NF_CHECK_EQ(st.current_frame_index, 0ULL);

    animator.resume();
    NF_CHECK(!animator.is_paused());
    st = animator.update(0.15f);
    NF_CHECK_EQ(st.current_frame_index, 1ULL);
}

NF_TEST(sprite_animator_notify_events) {
    SpriteAnimationClip clip("attack", SpriteLoopMode::Once);
    clip.add_frame("windup", 0.1f);
    clip.add_frame("strike", 0.1f, Vec2{0.0f, 0.0f}, "deal_damage");
    clip.add_frame("recovery", 0.1f);

    SpriteAnimator animator;
    animator.play(&clip);

    std::vector<std::string> notifies;
    animator.update(0.05f, &notifies);
    NF_CHECK(notifies.empty());

    // Advance to strike frame
    animator.update(0.08f, &notifies);
    NF_CHECK_EQ(notifies.size(), 1ULL);
    NF_CHECK_EQ(notifies[0], "deal_damage");

    // Advance within same frame -> does not fire duplicate notify
    notifies.clear();
    animator.update(0.02f, &notifies);
    NF_CHECK(notifies.empty());
}

NF_TEST(sprite_state_machine_parameter_transitions) {
    SpriteStateMachine2D sm;

    SpriteAnimationClip idle("idle", SpriteLoopMode::Loop);
    idle.add_frame("idle_0", 0.2f);
    idle.add_frame("idle_1", 0.2f);

    SpriteAnimationClip run("run", SpriteLoopMode::Loop);
    run.add_frame("run_0", 0.1f);
    run.add_frame("run_1", 0.1f);

    sm.add_state("idle", std::move(idle));
    sm.add_state("run", std::move(run));
    sm.set_default_state("idle");

    SpriteTransition to_run;
    to_run.to_state = "run";
    to_run.parameter_name = "speed";
    to_run.condition_type = SpriteConditionType::Greater;
    to_run.threshold = 0.1f;
    sm.add_transition("idle", to_run);

    SpriteTransition to_idle;
    to_idle.to_state = "idle";
    to_idle.parameter_name = "speed";
    to_idle.condition_type = SpriteConditionType::Less;
    to_idle.threshold = 0.1f;
    sm.add_transition("run", to_idle);

    // Initial state is idle
    auto st = sm.update(0.016f);
    NF_CHECK_EQ(sm.current_state_name(), "idle");
    NF_CHECK_EQ(st.region_name, "idle_0");

    // Player starts moving
    sm.set_float("speed", 5.0f);
    st = sm.update(0.016f);
    NF_CHECK_EQ(sm.current_state_name(), "run");
    NF_CHECK_EQ(st.region_name, "run_0");

    // Player stops
    sm.set_float("speed", 0.0f);
    st = sm.update(0.016f);
    NF_CHECK_EQ(sm.current_state_name(), "idle");
}

NF_TEST(sprite_state_machine_has_exit_time) {
    SpriteStateMachine2D sm;

    SpriteAnimationClip slash("slash", SpriteLoopMode::Once);
    slash.add_frame("slash_0", 0.1f);
    slash.add_frame("slash_1", 0.1f);

    SpriteAnimationClip idle("idle", SpriteLoopMode::Loop);
    idle.add_frame("idle_0", 0.2f);

    sm.add_state("slash", std::move(slash));
    sm.add_state("idle", std::move(idle));
    sm.set_default_state("slash");

    // Transition from slash back to idle once slash finishes
    SpriteTransition finish_to_idle;
    finish_to_idle.to_state = "idle";
    finish_to_idle.condition_type = SpriteConditionType::HasFinished;
    finish_to_idle.has_exit_time = true;
    sm.add_transition("slash", finish_to_idle);

    // Frame 0 of slash
    sm.update(0.05f);
    NF_CHECK_EQ(sm.current_state_name(), "slash");

    // Frame 1 of slash (not finished yet)
    sm.update(0.08f);
    NF_CHECK_EQ(sm.current_state_name(), "slash");

    // Past 0.2s -> slash finishes -> transitions to idle!
    sm.update(0.1f);
    NF_CHECK_EQ(sm.current_state_name(), "idle");
}
