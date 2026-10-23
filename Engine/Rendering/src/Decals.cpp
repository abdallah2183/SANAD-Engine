#include <NF/Rendering/Decals.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

// The unit cube's eight corners, in decal-local space. The shader tests
// `abs(local.xyz) <= 0.5` — this array is the CPU half of the same contract.
constexpr float kCorner[8][3] = {
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
    {-0.5f,  0.5f, -0.5f}, {0.5f,  0.5f, -0.5f},
    {-0.5f, -0.5f,  0.5f}, {0.5f, -0.5f,  0.5f},
    {-0.5f,  0.5f,  0.5f}, {0.5f,  0.5f,  0.5f},
};

} // namespace

Mat4 decal_world_matrix(const Mat4& entity_world, const Vec3& size) {
    // A zero or negative axis would collapse the box to a plane/line and every
    // fragment would fail the box test; clamp to a tiny positive extent so a
    // bad scene line degrades to "invisible decal", not NaNs in the inverse.
    const Vec3 safe_size{std::max(size.x, 1e-4f), std::max(size.y, 1e-4f),
                         std::max(size.z, 1e-4f)};
    return Mat4::scale(safe_size) * entity_world;
}

DecalInstance make_decal(u32 id, const Mat4& world, MaterialHandle material,
                         float opacity, bool enabled) {
    DecalInstance decal;
    decal.id = id;
    decal.world = world;
    decal.material = material;
    decal.opacity = std::clamp(opacity, 0.0f, 1.0f);
    decal.enabled = enabled;
    update_decal_inverse(decal);
    return decal;
}

void update_decal_inverse(DecalInstance& decal) {
    decal.inverse_world = decal.world.inverse();
}

DecalScreenRect decal_screen_rect(const DecalInstance& decal, const Mat4& view_proj,
                                  u32 width, u32 height) {
    DecalScreenRect rect;
    if (width == 0 || height == 0 || !decal.enabled) {
        return rect;
    }

    // Transform all eight corners once. `behind` is sticky: a single corner
    // behind the eye means the projected hull is not the box's true footprint,
    // and the safe answer is the whole viewport (the shader's per-pixel box
    // test still rejects everything outside the actual decal).
    bool have_corner = false;
    bool behind = false;
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    float min_z = 0.0f, max_z = 0.0f;

    for (const auto& c : kCorner) {
        const Vec4 world = decal.world * Vec4{c[0], c[1], c[2], 1.0f};
        const Vec4 clip = view_proj * world;
        if (clip.w <= 1e-6f) {
            behind = true;
            continue;
        }
        const float inv_w = 1.0f / clip.w;
        const float nx = clip.x * inv_w;
        const float ny = clip.y * inv_w;
        const float nz = clip.z * inv_w;
        if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)) {
            continue;
        }
        if (!have_corner) {
            min_x = max_x = nx;
            min_y = max_y = ny;
            min_z = max_z = nz;
            have_corner = true;
        } else {
            min_x = std::min(min_x, nx);
            max_x = std::max(max_x, nx);
            min_y = std::min(min_y, ny);
            max_y = std::max(max_y, ny);
            min_z = std::min(min_z, nz);
            max_z = std::max(max_z, nz);
        }
    }

    if (!have_corner) {
        return rect;
    }
    if (behind) {
        rect.x = 0;
        rect.y = 0;
        rect.width = static_cast<i32>(width);
        rect.height = static_cast<i32>(height);
        rect.visible = true;
        return rect;
    }

    // A box entirely in front of the near plane or entirely beyond the far
    // plane cannot contribute a fragment. Partially overlapping boxes are kept;
    // the per-pixel depth sample decides those.
    if (max_z < 0.0f || min_z > 1.0f) {
        return rect;
    }
    if (max_x < -1.0f || min_x > 1.0f || max_y < -1.0f || min_y > 1.0f) {
        return rect;
    }

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    // One pixel of padding absorbs the rasterizer's fill rule and float error
    // at the hull edge. Clamping afterwards keeps it inside the framebuffer.
    i32 x0 = static_cast<i32>(std::floor((min_x * 0.5f + 0.5f) * fw)) - 1;
    i32 x1 = static_cast<i32>(std::ceil((max_x * 0.5f + 0.5f) * fw)) + 1;
    i32 y0 = static_cast<i32>(std::floor((min_y * 0.5f + 0.5f) * fh)) - 1;
    i32 y1 = static_cast<i32>(std::ceil((max_y * 0.5f + 0.5f) * fh)) + 1;

    x0 = std::clamp(x0, 0, static_cast<i32>(width));
    x1 = std::clamp(x1, 0, static_cast<i32>(width));
    y0 = std::clamp(y0, 0, static_cast<i32>(height));
    y1 = std::clamp(y1, 0, static_cast<i32>(height));
    if (x1 <= x0 || y1 <= y0) {
        return rect;
    }

    rect.x = x0;
    rect.y = y0;
    rect.width = x1 - x0;
    rect.height = y1 - y0;
    rect.visible = true;
    return rect;
}

void sort_decals_far_to_near(std::span<DecalInstance> decals, const Vec3& camera_position) {
    std::stable_sort(decals.begin(), decals.end(),
                     [&](const DecalInstance& a, const DecalInstance& b) {
                         const float ax = a.world.m[3][0] - camera_position.x;
                         const float ay = a.world.m[3][1] - camera_position.y;
                         const float az = a.world.m[3][2] - camera_position.z;
                         const float bx = b.world.m[3][0] - camera_position.x;
                         const float by = b.world.m[3][1] - camera_position.y;
                         const float bz = b.world.m[3][2] - camera_position.z;
                         const float da = ax * ax + ay * ay + az * az;
                         const float db = bx * bx + by * by + bz * bz;
                         if (da > db) return true;
                         if (da < db) return false;
                         return a.id < b.id; // deterministic tie-break
                     });
}

} // namespace nf::rendering
