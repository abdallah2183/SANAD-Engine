#include <NF/Test/TestFramework.hpp>
#include <NF/Scene2D/Parallax.hpp>
#include <NF/Scene2D/NineSlice.hpp>

using namespace nf;
using namespace nf::scene2d;

NF_TEST(parallax_layer_scroll_ratio_and_offsets) {
    ParallaxManager mgr;

    ParallaxLayer mountains;
    mountains.name = "Mountains";
    mountains.scroll_ratio = Vec2(0.2f, 0.2f); // Far background moves slowly
    mountains.tile_size = Vec2(50.0f, 50.0f);
    mgr.add_layer(mountains);

    // Camera at (0, 0)
    Vec2 o0 = mgr.calculate_layer_offset(0, Vec2(0.0f, 0.0f), 0.0f);
    NF_CHECK_NEAR(o0.x, 0.0f, 1e-4f);

    // Camera moves to (100, 0)
    // Offset = 100 * (1 - 0.2) = 80.0
    Vec2 o1 = mgr.calculate_layer_offset(0, Vec2(100.0f, 0.0f), 0.0f);
    NF_CHECK_NEAR(o1.x, 80.0f, 1e-4f);
}

NF_TEST(parallax_auto_scroll_velocity) {
    ParallaxManager mgr;

    ParallaxLayer clouds;
    clouds.name = "Clouds";
    clouds.scroll_ratio = Vec2(1.0f, 1.0f); // Stays in place relative to world except auto scroll
    clouds.auto_scroll = Vec2(15.0f, 0.0f); // 15 units/sec
    mgr.add_layer(clouds);

    Vec2 o_t0 = mgr.calculate_layer_offset(0, Vec2(0.0f, 0.0f), 0.0f);
    NF_CHECK_NEAR(o_t0.x, 0.0f, 1e-4f);

    Vec2 o_t2 = mgr.calculate_layer_offset(0, Vec2(0.0f, 0.0f), 2.0f);
    NF_CHECK_NEAR(o_t2.x, 30.0f, 1e-4f);
}

NF_TEST(parallax_visible_tiles_coverage) {
    ParallaxManager mgr;

    ParallaxLayer bg;
    bg.tile_size = Vec2(20.0f, 20.0f);
    bg.repeat_x = true;
    bg.repeat_y = false;
    mgr.add_layer(bg);

    i32 min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    mgr.calculate_visible_tiles(0, Vec2(50.0f, 0.0f), Vec2(60.0f, 40.0f), 0.0f, min_x, max_x, min_y, max_y);

    // View spans [50 - 30, 50 + 30] = [20, 80]
    // Covered tile indices: at 20 -> 1, at 80 -> 4
    NF_CHECK(min_x <= 1);
    NF_CHECK(max_x >= 4);
    NF_CHECK_EQ(min_y, 0);
    NF_CHECK_EQ(max_y, 0);
}

NF_TEST(nine_slice_quad_generation_and_corners) {
    NineSliceMargins margins;
    margins.left = 10.0f;
    margins.right = 10.0f;
    margins.top = 10.0f;
    margins.bottom = 10.0f;

    UVRect uv;
    uv.u0 = 0.0f; uv.v0 = 0.0f;
    uv.u1 = 1.0f; uv.v1 = 1.0f;

    NineSliceSprite sprite(margins, uv);
    Vec2 target_size(100.0f, 60.0f);
    Vec2 center(0.0f, 0.0f);

    auto quads = sprite.build_quads(target_size, center);
    NF_CHECK_EQ(quads.size(), 9u);

    // Check bottom-left corner quad (quad index 6)
    const auto& q_bl = quads[6];
    const f32 w_bl = q_bl.pos_max.x - q_bl.pos_min.x;
    const f32 h_bl = q_bl.pos_max.y - q_bl.pos_min.y;
    NF_CHECK_NEAR(w_bl, 10.0f, 1e-4f);
    NF_CHECK_NEAR(h_bl, 10.0f, 1e-4f);

    // Check center quad (quad index 4)
    const auto& q_center = quads[4];
    const f32 w_center = q_center.pos_max.x - q_center.pos_min.x;
    const f32 h_center = q_center.pos_max.y - q_center.pos_min.y;
    // Target w 100 - 20 = 80, Target h 60 - 20 = 40
    NF_CHECK_NEAR(w_center, 80.0f, 1e-4f);
    NF_CHECK_NEAR(h_center, 40.0f, 1e-4f);
}

NF_TEST(nine_slice_clamping_on_small_targets) {
    NineSliceMargins margins;
    margins.left = 15.0f;
    margins.right = 15.0f;
    margins.top = 10.0f;
    margins.bottom = 10.0f;

    NineSliceSprite sprite(margins, UVRect{});

    // Target width 10 < sum of margins (30)
    Vec2 small_size(10.0f, 10.0f);
    auto quads = sprite.build_quads(small_size, Vec2(0.0f, 0.0f));

    for (const auto& q : quads) {
        NF_CHECK(q.pos_max.x >= q.pos_min.x);
        NF_CHECK(q.pos_max.y >= q.pos_min.y);
    }
}
