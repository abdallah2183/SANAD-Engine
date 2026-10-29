#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Easing.hpp>
#include <NF/Core/Curve.hpp>

using namespace nf;

NF_TEST(easing_boundaries_and_monotonicity) {
    // All easing functions should evaluate to 0 at t=0 and 1 at t=1 (approx)
    for (u8 i = 0; i < static_cast<u8>(EaseType::Count); ++i) {
        EaseType ease = static_cast<EaseType>(i);
        float v0 = evaluate_ease(ease, 0.0f);
        float v1 = evaluate_ease(ease, 1.0f);

        NF_CHECK_NEAR(v0, 0.0f, 1e-4f);
        NF_CHECK_NEAR(v1, 1.0f, 1e-4f);

        // Clamping check
        float v_neg = evaluate_ease(ease, -0.5f);
        float v_pos = evaluate_ease(ease, 1.5f);
        NF_CHECK_NEAR(v_neg, 0.0f, 1e-4f);
        NF_CHECK_NEAR(v_pos, 1.0f, 1e-4f);
    }

    // Linear should be strictly identity
    NF_CHECK_NEAR(evaluate_ease(EaseType::Linear, 0.5f), 0.5f, 1e-5f);
    // InQuad at 0.5 is 0.25
    NF_CHECK_NEAR(evaluate_ease(EaseType::InQuad, 0.5f), 0.25f, 1e-5f);
    // OutQuad at 0.5 is 0.75
    NF_CHECK_NEAR(evaluate_ease(EaseType::OutQuad, 0.5f), 0.75f, 1e-5f);
}

NF_TEST(curve_linear_interpolation) {
    FloatCurve curve;
    curve.add_key(0.0f, 0.0f, TangentMode::Linear);
    curve.add_key(2.0f, 100.0f, TangentMode::Linear);

    NF_CHECK_EQ(curve.key_count(), 2u);
    NF_CHECK_NEAR(curve.evaluate(0.0f), 0.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(1.0f), 50.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(2.0f), 100.0f, 1e-5f);

    // Clamping past bounds
    NF_CHECK_NEAR(curve.evaluate(-1.0f), 0.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(3.0f), 100.0f, 1e-5f);
}

NF_TEST(curve_constant_stepping) {
    FloatCurve curve;
    curve.add_key(0.0f, 10.0f, TangentMode::Constant);
    curve.add_key(1.0f, 20.0f, TangentMode::Constant);
    curve.add_key(2.0f, 30.0f, TangentMode::Constant);

    NF_CHECK_NEAR(curve.evaluate(0.0f), 10.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(0.99f), 10.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(1.0f), 20.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(1.5f), 20.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(2.0f), 30.0f, 1e-5f);
}

NF_TEST(curve_hermite_smooth_and_presets) {
    FloatCurve curve = FloatCurve::ease_in_out(0.0f, 10.0f, 2.0f);

    NF_CHECK_EQ(curve.key_count(), 2u);
    NF_CHECK_NEAR(curve.evaluate(0.0f), 0.0f, 1e-5f);
    // Midpoint of ease in out should be halfway
    NF_CHECK_NEAR(curve.evaluate(1.0f), 5.0f, 1e-5f);
    NF_CHECK_NEAR(curve.evaluate(2.0f), 10.0f, 1e-5f);
}

NF_TEST(curve_loop_and_pingpong_extrapolation) {
    FloatCurve curve;
    curve.add_key(0.0f, 0.0f, TangentMode::Linear);
    curve.add_key(1.0f, 10.0f, TangentMode::Linear);

    curve.post_extrapolation = ExtrapolationMode::Loop;
    NF_CHECK_NEAR(curve.evaluate(1.5f), 5.0f, 1e-4f);
    NF_CHECK_NEAR(curve.evaluate(2.5f), 5.0f, 1e-4f);

    curve.post_extrapolation = ExtrapolationMode::PingPong;
    NF_CHECK_NEAR(curve.evaluate(1.5f), 5.0f, 1e-4f);
    NF_CHECK_NEAR(curve.evaluate(1.8f), 2.0f, 1e-4f);
}
