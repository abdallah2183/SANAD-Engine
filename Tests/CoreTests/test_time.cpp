// CoreTests/test_time.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Time.hpp>

using namespace nf;

NF_TEST(test_clock_elapsed) {
    Clock c;
    Time::sleep_ms(10);
    f64 elapsed = c.elapsed_seconds();
    NF_CHECK(elapsed >= 0.008); // At least 8ms with some tolerance
}

NF_TEST(test_time_now) {
    f64 t1 = Time::now();
    Time::sleep_ms(1);
    f64 t2 = Time::now();
    NF_CHECK(t2 > t1);
}

NF_TEST(test_timer) {
    Timer t;
    t.start();
    Time::sleep_ms(5);
    f32 delta = t.tick();
    NF_CHECK(delta > 0.0f);
}
