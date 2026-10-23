#include <NF/Rendering/RenderScale.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

f32 sanitize_render_scale(f32 scale) {
    if (!std::isfinite(scale)) {
        return 1.0f;
    }
    return std::clamp(scale, kMinRenderScale, kMaxRenderScale);
}

RenderTargetSize scaled_render_size(u32 output_width, u32 output_height, f32 scale) {
    if (output_width == 0 || output_height == 0) {
        return {};
    }
    const f32 clean = sanitize_render_scale(scale);
    const auto axis = [clean](u32 value) {
        // llround is round-half-away-from-zero. The exact tie is not a special
        // case here: either neighbouring texel is a valid supersample, and a
        // documented rule beats a platform's default rounding mode.
        const long long scaled =
            std::llround(static_cast<double>(value) * static_cast<double>(clean));
        return static_cast<u32>(
            std::clamp<long long>(scaled, 1, static_cast<long long>(kMaxRenderTargetDimension)));
    };
    return RenderTargetSize{axis(output_width), axis(output_height)};
}

} // namespace nf::rendering
