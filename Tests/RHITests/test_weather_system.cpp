// Tests/RHITests/test_weather_system.cpp — Atmospheric weather & height fog tests (Phase 115)

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/Weather.hpp>

#include <cmath>

using namespace nf;
using namespace nf::rendering;

NF_TEST(weather_default_initialization) {
    WeatherSystem ws;
    NF_CHECK_EQ(static_cast<u8>(ws.current_weather_type()), static_cast<u8>(WeatherType::Clear));
    NF_CHECK(!ws.is_transitioning());
    NF_CHECK_NEAR(ws.transition_progress(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(ws.ground_wetness(), 0.0f, 1e-4f);
    NF_CHECK(ws.current_preset().fog.base_density > 0.0f);
}

NF_TEST(weather_factory_presets) {
    const WeatherPreset clear_p = WeatherSystem::get_default_preset(WeatherType::Clear);
    const WeatherPreset rain_p  = WeatherSystem::get_default_preset(WeatherType::Rain);
    const WeatherPreset fog_p   = WeatherSystem::get_default_preset(WeatherType::Foggy);
    const WeatherPreset storm_p = WeatherSystem::get_default_preset(WeatherType::Thunderstorm);

    NF_CHECK_NEAR(clear_p.precipitation_rate, 0.0f, 1e-4f);
    NF_CHECK(rain_p.precipitation_rate > 0.5f);
    NF_CHECK(rain_p.wetness_accumulation_rate > 0.0f);
    NF_CHECK(fog_p.fog.base_density > clear_p.fog.base_density);
    NF_CHECK(storm_p.wind_strength > rain_p.wind_strength);
}

NF_TEST(weather_instant_transition) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Rain, 0.0f);

    NF_CHECK_EQ(static_cast<u8>(ws.current_weather_type()), static_cast<u8>(WeatherType::Rain));
    NF_CHECK_EQ(static_cast<u8>(ws.target_weather_type()), static_cast<u8>(WeatherType::Rain));
    NF_CHECK(!ws.is_transitioning());
    NF_CHECK_NEAR(ws.transition_progress(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(ws.current_preset().precipitation_rate, 0.75f, 1e-4f);
}

NF_TEST(weather_smooth_transition_interpolation) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Foggy, 2.0f);

    NF_CHECK(ws.is_transitioning());
    NF_CHECK_NEAR(ws.transition_progress(), 0.0f, 1e-4f);

    ws.update(1.0f); // 50% transition
    NF_CHECK(ws.is_transitioning());
    NF_CHECK_NEAR(ws.transition_progress(), 0.5f, 1e-4f);

    // Fog density should be strictly between Clear and Foggy
    const f32 clear_dense = WeatherSystem::get_default_preset(WeatherType::Clear).fog.base_density;
    const f32 fog_dense = WeatherSystem::get_default_preset(WeatherType::Foggy).fog.base_density;
    NF_CHECK(ws.current_preset().fog.base_density > clear_dense);
    NF_CHECK(ws.current_preset().fog.base_density < fog_dense);

    ws.update(1.1f); // Finish transition
    NF_CHECK(!ws.is_transitioning());
    NF_CHECK_NEAR(ws.transition_progress(), 1.0f, 1e-4f);
    NF_CHECK_EQ(static_cast<u8>(ws.current_weather_type()), static_cast<u8>(WeatherType::Foggy));
    NF_CHECK_NEAR(ws.current_preset().fog.base_density, fog_dense, 1e-4f);
}

NF_TEST(weather_height_fog_distance_culling) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Foggy, 0.0f);

    const Vec3 cam{0.0f, 0.0f, 0.0f};
    const Vec3 near_target{0.0f, 0.0f, 0.2f}; // within start_distance (0.5m)
    const Vec3 mid_target{0.0f, 0.0f, 25.0f};
    const Vec3 far_target{0.0f, 0.0f, 100.0f};

    const f32 fog_near = ws.compute_fog_factor(cam, near_target);
    const f32 fog_mid  = ws.compute_fog_factor(cam, mid_target);
    const f32 fog_far  = ws.compute_fog_factor(cam, far_target);

    NF_CHECK_NEAR(fog_near, 0.0f, 1e-4f);
    NF_CHECK(fog_mid > 0.0f);
    NF_CHECK(fog_far > fog_mid);
    NF_CHECK(fog_far <= 1.0f);
}

NF_TEST(weather_height_fog_exponential_falloff) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Foggy, 0.0f);

    // Ground ray at y = 0
    const Vec3 ground_start{0.0f, 0.0f, 0.0f};
    const Vec3 ground_end{50.0f, 0.0f, 0.0f};
    const f32 ground_fog = ws.compute_fog_factor(ground_start, ground_end);

    // High altitude ray at y = 150
    const Vec3 high_start{0.0f, 150.0f, 0.0f};
    const Vec3 high_end{50.0f, 150.0f, 0.0f};
    const f32 high_fog = ws.compute_fog_factor(high_start, high_end);

    NF_CHECK(ground_fog > 0.5f);
    NF_CHECK(high_fog < ground_fog * 0.1f);
}

NF_TEST(weather_sun_inscattering_glow) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Clear, 0.0f);

    const Vec3 sun_dir{0.0f, 0.7071f, 0.7071f};
    const Vec3 direct_ray = sun_dir;
    const Vec3 perp_ray{1.0f, 0.0f, 0.0f};

    const Vec3 direct_color = ws.compute_fog_color(direct_ray, sun_dir);
    const Vec3 perp_color   = ws.compute_fog_color(perp_ray, sun_dir);

    // Direct ray should pick up significantly more sun color
    const Vec3 sun_color = ws.current_preset().sun_color;
    const f32 direct_sun_diff = (direct_color - sun_color).length();
    const f32 perp_sun_diff   = (perp_color - sun_color).length();

    NF_CHECK(direct_sun_diff < perp_sun_diff);
}

NF_TEST(weather_ground_wetness_accumulation) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Rain, 0.0f);
    NF_CHECK_NEAR(ws.ground_wetness(), 0.0f, 1e-4f);

    ws.update(2.0f);
    NF_CHECK(ws.ground_wetness() > 0.1f);

    ws.update(20.0f);
    NF_CHECK_NEAR(ws.ground_wetness(), 1.0f, 1e-4f); // clamped at 1.0
}

NF_TEST(weather_ground_drying) {
    WeatherSystem ws;
    ws.set_ground_wetness(0.5f);
    ws.set_weather(WeatherType::Clear, 0.0f); // accumulation rate is 0
    ws.set_drying_speed(0.1f);

    ws.update(2.0f);
    NF_CHECK_NEAR(ws.ground_wetness(), 0.3f, 0.02f);

    ws.update(5.0f);
    NF_CHECK_NEAR(ws.ground_wetness(), 0.0f, 1e-4f); // clamped at 0.0
}

NF_TEST(weather_dynamic_wind_gusts) {
    WeatherSystem ws;
    ws.set_weather(WeatherType::Overcast, 0.0f);

    const Vec3 initial_wind = ws.current_wind_vector();
    NF_CHECK(initial_wind.length() > 0.0f);

    ws.update(0.5f);
    const Vec3 wind_t1 = ws.current_wind_vector();

    ws.update(0.75f);
    const Vec3 wind_t2 = ws.current_wind_vector();

    // Wind direction should be aligned with preset direction
    NF_CHECK_NEAR(wind_t1.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(wind_t1.z, 0.0f, 1e-4f);
    NF_CHECK(wind_t1.x > 0.0f);
    // Gust oscillation should create dynamic variation
    NF_CHECK(std::abs(wind_t1.x - wind_t2.x) > 0.01f || std::abs(wind_t1.x - initial_wind.x) > 0.01f);
}
