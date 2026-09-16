// RHITests — time-of-day cycle: elevation, sun path, light/sky palettes.
//
// Pure CPU (no device): pins the solar model the renderer consumes.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/TimeOfDay.hpp>

#include <cmath>

using namespace nf;
using namespace nf::rendering;

NF_TEST(timeofday_elevation_landmarks) {
    TimeOfDay tod;
    tod.set_time_hours(12.0f);
    NF_CHECK_NEAR(tod.sun_elevation(), 1.0f, 1e-5f);
    tod.set_time_hours(0.0f);
    NF_CHECK_NEAR(tod.sun_elevation(), -1.0f, 1e-5f);
    tod.set_time_hours(6.0f);
    NF_CHECK_NEAR(tod.sun_elevation(), 0.0f, 1e-5f);
    tod.set_time_hours(18.0f);
    NF_CHECK_NEAR(tod.sun_elevation(), 0.0f, 1e-5f);
    NF_CHECK(tod.is_day() == false);
    tod.set_time_hours(9.0f);
    NF_CHECK(tod.is_day());
}

NF_TEST(timeofday_wraps_and_advances) {
    TimeOfDay tod;
    tod.set_time_hours(25.5f);
    NF_CHECK_NEAR(tod.time_hours(), 1.5f, 1e-5f);
    tod.set_time_hours(-1.0f);
    NF_CHECK_NEAR(tod.time_hours(), 23.0f, 1e-5f);

    tod.set_day_length_seconds(240.0f); // 10 game-minutes per real second
    tod.set_time_hours(0.0f);
    tod.advance(60.0f); // quarter cycle = 6 game-hours
    NF_CHECK_NEAR(tod.time_hours(), 6.0f, 1e-4f);

    tod.set_day_length_seconds(0.0f); // frozen
    tod.advance(3600.0f);
    NF_CHECK_NEAR(tod.time_hours(), 6.0f, 1e-5f);
}

NF_TEST(timeofday_sun_path_faces_down_by_day) {
    TimeOfDay tod;
    for (float h : {7.0f, 9.0f, 12.0f, 15.0f, 17.0f}) {
        tod.set_time_hours(h);
        const Vec3 d = tod.sun_direction();
        NF_CHECK_NEAR(d.length(), 1.0f, 1e-5f);
        NF_CHECK(d.y < 0.0f); // travel direction points down
    }
    // Noon sun is nearly overhead (mostly -Y).
    tod.set_time_hours(12.0f);
    NF_CHECK(tod.sun_direction().y < -0.9f);
    // Morning/evening suns lean east/west.
    tod.set_time_hours(7.0f);
    const float east_x = tod.sun_direction().x;
    tod.set_time_hours(17.0f);
    const float west_x = tod.sun_direction().x;
    NF_CHECK(east_x * west_x < 0.0f); // opposite sides
}

NF_TEST(timeofday_light_day_brighter_than_night) {
    TimeOfDay tod;
    tod.set_time_hours(12.0f);
    const DirectionalLight noon = tod.make_light();
    NF_CHECK(noon.enabled);
    tod.set_time_hours(0.0f);
    const DirectionalLight midnight = tod.make_light();
    NF_CHECK(midnight.enabled);
    NF_CHECK(midnight.intensity < noon.intensity);
    NF_CHECK(midnight.intensity > 0.0f); // moonlight, not pitch black
    // Noon light is near-white; sunrise warms up.
    NF_CHECK(noon.color.x > 0.9f && noon.color.y > 0.9f);
    tod.set_time_hours(6.5f);
    const DirectionalLight dawn = tod.make_light();
    NF_CHECK(dawn.color.x >= dawn.color.z); // warm: red dominates blue
}

NF_TEST(timeofday_sky_palettes_blend) {
    TimeOfDay tod;
    tod.set_time_hours(12.0f);
    const SkyParams noon = tod.make_sky();
    NF_CHECK(noon.enabled);
    NF_CHECK_NEAR(noon.horizon.x, 0.62f, 1e-5f); // default day look

    tod.set_time_hours(0.0f);
    const SkyParams night = tod.make_sky();
    NF_CHECK(night.horizon.x < noon.horizon.x); // darker
    NF_CHECK(night.zenith.y < noon.zenith.y);

    // Sunset sits between day and night.
    tod.set_time_hours(17.5f);
    const SkyParams dusk = tod.make_sky();
    NF_CHECK(dusk.horizon.x > night.horizon.x); // ember brighter than night
    NF_CHECK(dusk.horizon.y < noon.horizon.y); // warmer/redder than noon
}
