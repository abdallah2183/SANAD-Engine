// Editor thumbnail downsample: the CPU half of the P3 preview path.
//
// The shell decodes an image and uploads the result; this module decides what
// that result is. The contract worth pinning down: the longest side lands at or
// under max_dim, aspect is preserved, a small source is copied rather than
// magnified, and averaging is exact for a flat colour.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/PreviewPixels.hpp>

#include <vector>

using namespace nf;

namespace {

std::vector<u8> make_flat(int w, int h, u8 r, u8 g, u8 b, u8 a) {
    std::vector<u8> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < px.size(); i += 4u) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

} // namespace

NF_TEST(preview_downsample_rejects_bad_input) {
    const std::vector<u8> flat = make_flat(4, 4, 10, 20, 30, 255);
    NF_CHECK(!editor::downsample_rgba(nullptr, 4, 4, 128).valid());
    NF_CHECK(!editor::downsample_rgba(flat.data(), 0, 4, 128).valid());
    NF_CHECK(!editor::downsample_rgba(flat.data(), 4, -1, 128).valid());
    NF_CHECK(!editor::downsample_rgba(flat.data(), 4, 4, 0).valid());
    NF_CHECK(!editor::downsample_rgba(flat.data(), 4, 4, -8).valid());
}

NF_TEST(preview_downsample_copies_when_already_small) {
    // 64x48 is under the 128 cap: the result must be the same pixels, not an
    // averaged (and thus blurred) copy of them.
    const std::vector<u8> src = make_flat(64, 48, 200, 100, 50, 255);
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), 64, 48, 128);
    NF_CHECK(out.valid());
    NF_CHECK_EQ(out.width, 64);
    NF_CHECK_EQ(out.height, 48);
    NF_CHECK_EQ(out.rgba.size(), src.size());
    NF_CHECK(out.rgba == src);
}

NF_TEST(preview_downsample_exact_power_of_two) {
    // 256x256 -> 128x128, scale 2: every output pixel is the average of a 2x2
    // box of identical pixels, so a flat image stays exactly flat.
    const std::vector<u8> src = make_flat(256, 256, 60, 120, 180, 255);
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), 256, 256, 128);
    NF_CHECK(out.valid());
    NF_CHECK_EQ(out.width, 128);
    NF_CHECK_EQ(out.height, 128);
    for (size_t i = 0; i < out.rgba.size(); i += 4u) {
        NF_CHECK_EQ(out.rgba[i + 0], 60);
        NF_CHECK_EQ(out.rgba[i + 1], 120);
        NF_CHECK_EQ(out.rgba[i + 2], 180);
        NF_CHECK_EQ(out.rgba[i + 3], 255);
    }
}

NF_TEST(preview_downsample_uses_ceiling_scale) {
    // 300 wide with a 128 cap: floor(300/128) = 2 would give a 150-wide result
    // and break "at most max_dim". Ceiling gives scale 3 and a 100-wide result.
    const std::vector<u8> src = make_flat(300, 300, 255, 0, 0, 255);
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), 300, 300, 128);
    NF_CHECK(out.valid());
    NF_CHECK(out.width <= 128);
    NF_CHECK(out.height <= 128);
    NF_CHECK_EQ(out.width, 100);
    NF_CHECK_EQ(out.height, 100);
}

NF_TEST(preview_downsample_preserves_aspect) {
    // A 512x256 image scaled by 4 in both axes stays 2:1; the longest side is
    // the only one the contract constrains.
    const std::vector<u8> src = make_flat(512, 256, 1, 2, 3, 4);
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), 512, 256, 128);
    NF_CHECK(out.valid());
    NF_CHECK_EQ(out.width, 128);
    NF_CHECK_EQ(out.height, 64);
    NF_CHECK_EQ(out.width, out.height * 2);
}

NF_TEST(preview_downsample_averages_a_gradient) {
    // Alternating black/white columns scaled 2:1: each 2-pixel box holds one of
    // each, so every output pixel is one mid-grey (127 or 128 depending on how
    // the integer divide rounds) — not a banded edge.
    const int w = 8;
    const int h = 2;
    std::vector<u8> src(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const u8 v = (x % 2 == 0) ? 0 : 255;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            src[i + 0] = v;
            src[i + 1] = v;
            src[i + 2] = v;
            src[i + 3] = 255;
        }
    }
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), w, h, 4);
    NF_CHECK(out.valid());
    NF_CHECK_EQ(out.width, 4);
    NF_CHECK_EQ(out.height, 1);
    for (size_t i = 0; i < out.rgba.size(); i += 4u) {
        NF_CHECK(out.rgba[i + 0] == 127 || out.rgba[i + 0] == 128);
        NF_CHECK_EQ(out.rgba[i + 3], 255);
    }
}

NF_TEST(preview_downsample_alpha_is_averaged_too) {
    // Alpha is a channel like any other: alternating 0/255 columns must average
    // to mid-grey, not be taken from the first pixel of each box.
    const int w = 4;
    const int h = 2;
    std::vector<u8> src(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            src[i + 0] = 255;
            src[i + 1] = 0;
            src[i + 2] = 0;
            src[i + 3] = (x % 2 == 0) ? 0 : 255;
        }
    }
    const editor::PreviewPixels out = editor::downsample_rgba(src.data(), w, h, 2);
    NF_CHECK(out.valid());
    NF_CHECK_EQ(out.width, 2);
    NF_CHECK_EQ(out.height, 1);
    for (size_t i = 0; i < out.rgba.size(); i += 4u) {
        NF_CHECK_EQ(out.rgba[i + 3], 127);
    }
}
