#include <NF/Test/TestFramework.hpp>
#include <NF/Core/TimerManager.hpp>

using namespace nf;

NF_TEST(timer_timeout_and_clear) {
    TimerManager timers;
    bool fired = false;

    TimerHandle h = timers.set_timeout(0.5f, [&fired]() {
        fired = true;
    });

    NF_CHECK(h.valid());
    NF_CHECK(timers.is_timer_active(h));
    NF_CHECK_EQ(timers.active_count(), 1u);

    // Advance 0.2s - should not have fired yet
    timers.update(0.2f);
    NF_CHECK(!fired);
    NF_CHECK(timers.is_timer_active(h));
    NF_CHECK(timers.get_time_remaining(h) <= 0.31f);

    // Advance 0.35s - fires
    timers.update(0.35f);
    NF_CHECK(fired);
    NF_CHECK(!timers.is_timer_active(h));
    NF_CHECK_EQ(timers.active_count(), 0u);
}

NF_TEST(timer_interval_and_cancel) {
    TimerManager timers;
    int count = 0;

    TimerHandle h = timers.set_interval(0.1f, [&count]() {
        count++;
    });

    timers.update(0.15f);
    NF_CHECK_EQ(count, 1);
    NF_CHECK(timers.is_timer_active(h));

    timers.update(0.1f);
    NF_CHECK_EQ(count, 2);

    bool cleared = timers.clear_timer(h);
    NF_CHECK(cleared);
    NF_CHECK(!timers.is_timer_active(h));

    timers.update(0.5f);
    NF_CHECK_EQ(count, 2); // No further increments
}

NF_TEST(timer_pause_and_resume) {
    TimerManager timers;
    bool fired = false;

    TimerHandle h = timers.set_timeout(0.2f, [&fired]() {
        fired = true;
    });

    timers.pause_timer(h);
    NF_CHECK(timers.is_timer_paused(h));
    NF_CHECK(!timers.is_timer_active(h));

    // Advance while paused
    timers.update(0.5f);
    NF_CHECK(!fired);

    // Resume
    timers.resume_timer(h);
    NF_CHECK(timers.is_timer_active(h));

    timers.update(0.25f);
    NF_CHECK(fired);
}

NF_TEST(timer_time_dilation) {
    TimerManager timers;
    timers.set_time_dilation(0.5f); // Half speed
    bool fired = false;

    timers.set_timeout(0.2f, [&fired]() {
        fired = true;
    });

    // 0.2s at 0.5x dilation gives 0.1s effective progress
    timers.update(0.2f);
    NF_CHECK(!fired);

    // Another 0.25s at 0.5x gives 0.125s -> total 0.225s, so fires!
    timers.update(0.25f);
    NF_CHECK(fired);
}

NF_TEST(timer_reentrancy_safe) {
    TimerManager timers;
    int phase = 0;

    timers.set_timeout(0.1f, [&timers, &phase]() {
        phase = 1;
        // Schedule next timer inside callback
        timers.set_timeout(0.1f, [&phase]() {
            phase = 2;
        });
    });

    timers.update(0.15f);
    NF_CHECK_EQ(phase, 1);

    timers.update(0.15f);
    NF_CHECK_EQ(phase, 2);
}
