#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Tween.hpp>

using namespace nf;

NF_TEST(scalar_tween_progress_and_completion) {
    float val = 0.0f;
    bool completed = false;

    ScalarTween tween(1, 0.0f, 100.0f, 1.0f, EaseType::Linear);
    tween.on_update([&val](float v) {
        val = v;
    });
    tween.on_complete([&completed]() {
        completed = true;
    });

    NF_CHECK(!tween.is_finished());
    tween.update(0.5f);
    NF_CHECK_NEAR(val, 50.0f, 1e-4f);
    NF_CHECK(!completed);

    tween.update(0.5f);
    NF_CHECK_NEAR(val, 100.0f, 1e-4f);
    NF_CHECK(completed);
    NF_CHECK(tween.is_finished());
}

NF_TEST(scalar_tween_ping_pong_loops) {
    float val = 0.0f;
    int completed_loops = 0;

    ScalarTween tween(2, 0.0f, 10.0f, 1.0f, EaseType::Linear);
    tween.set_loops(2, true); // ping pong forward then back
    tween.on_update([&val](float v) {
        val = v;
    });
    tween.on_complete([&completed_loops]() {
        completed_loops = 2;
    });

    tween.update(1.0f); // End of forward loop
    NF_CHECK_NEAR(val, 10.0f, 1e-4f);
    NF_CHECK(!tween.is_finished());

    tween.update(0.5f); // Halfway back
    NF_CHECK_NEAR(val, 5.0f, 1e-4f);

    tween.update(0.5f); // Finish reverse loop
    NF_CHECK_NEAR(val, 0.0f, 1e-4f);
    NF_CHECK(tween.is_finished());
    NF_CHECK_EQ(completed_loops, 2);
}

NF_TEST(tween_manager_lifecycle) {
    TweenManager& mgr = TweenManager::instance();
    mgr.clear();
    NF_CHECK_EQ(mgr.active_count(), 0u);

    float target_f = 0.0f;
    uint64_t id1 = mgr.to(&target_f, 50.0f, 1.0f, EaseType::Linear);
    NF_CHECK_EQ(mgr.active_count(), 1u);

    Vec3 target_v{0.0f, 0.0f, 0.0f};
    uint64_t id2 = mgr.to(&target_v, Vec3{10.0f, 20.0f, 30.0f}, 1.0f, EaseType::Linear);
    NF_CHECK_EQ(mgr.active_count(), 2u);
    NF_CHECK(id2 > 0);

    mgr.update(0.5f);
    NF_CHECK_NEAR(target_f, 25.0f, 1e-4f);
    NF_CHECK_NEAR(target_v.x, 5.0f, 1e-4f);
    NF_CHECK_NEAR(target_v.y, 10.0f, 1e-4f);
    NF_CHECK_NEAR(target_v.z, 15.0f, 1e-4f);

    // Cancel id1
    NF_CHECK(mgr.cancel(id1));
    mgr.update(0.5f);
    // target_f stopped at 25.0, target_v reached 10, 20, 30 and finished
    NF_CHECK_NEAR(target_f, 25.0f, 1e-4f);
    NF_CHECK_NEAR(target_v.x, 10.0f, 1e-4f);

    // All finished or cancelled tweens are purged
    NF_CHECK_EQ(mgr.active_count(), 0u);
}
