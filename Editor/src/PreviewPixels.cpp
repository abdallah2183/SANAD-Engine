#include <NF/Editor/PreviewPixels.hpp>

#include <algorithm>

namespace nf::editor {

PreviewPixels downsample_rgba(const u8* rgba, int width, int height, int max_dim) {
    PreviewPixels out;
    if (rgba == nullptr || width <= 0 || height <= 0 || max_dim <= 0) {
        return out;
    }
    const int longest = (width >= height) ? width : height;
    if (longest <= max_dim) {
        // Already small enough: hand back the bytes untouched.
        out.width = width;
        out.height = height;
        out.rgba.assign(rgba, rgba + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        return out;
    }
    // Integer scale only: a fractional box boundary would average a partial
    // row at the right/bottom edge and shift the image by a fraction of a
    // pixel. The scale is the CEILING of longest/max_dim rather than the floor
    // — floor of 300/128 is 2, which yields a 150-pixel wide result and
    // breaks the "at most max_dim" contract.
    const int scale = (longest + max_dim - 1) / max_dim;
    if (scale <= 1) {
        out.width = width;
        out.height = height;
        out.rgba.assign(rgba, rgba + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        return out;
    }
    out.width = width / scale;
    out.height = height / scale;
    if (out.width <= 0 || out.height <= 0) {
        return PreviewPixels{};
    }
    out.rgba.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u, 0);
    const size_t row_src = static_cast<size_t>(width) * 4u;
    const size_t box = static_cast<size_t>(scale) * static_cast<size_t>(scale);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const u8* box_origin = rgba + (static_cast<size_t>(y) * scale) * row_src +
                                   (static_cast<size_t>(x) * scale) * 4u;
            u32 sum[4]{};
            for (int by = 0; by < scale; ++by) {
                const u8* row = box_origin + static_cast<size_t>(by) * row_src;
                for (int bx = 0; bx < scale; ++bx) {
                    const u8* px = row + static_cast<size_t>(bx) * 4u;
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += px[c];
                    }
                }
            }
            u8* dst = out.rgba.data() +
                      (static_cast<size_t>(y) * static_cast<size_t>(out.width) +
                       static_cast<size_t>(x)) *
                          4u;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<u8>(sum[c] / box);
            }
        }
    }
    return out;
}

} // namespace nf::editor
