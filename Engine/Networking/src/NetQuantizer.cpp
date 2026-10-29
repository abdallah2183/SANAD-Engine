#include <NF/Networking/NetQuantizer.hpp>
#include <cmath>
#include <algorithm>

namespace nf::net {

constexpr f32 INV_SQRT_TWO = 0.7071067811865475244f;

u32 NetQuantizer::quantize_float(f32 val, f32 min_val, f32 max_val, u32 bit_count) {
    if (bit_count == 0) {
        return 0;
    }
    bit_count = std::min(bit_count, 32u);
    const u32 max_int = (bit_count == 32) ? 0xFFFFFFFFu : ((1u << bit_count) - 1u);

    if (max_val <= min_val) {
        return 0;
    }

    const f32 clamped = std::clamp(val, min_val, max_val);
    const f32 normalized = (clamped - min_val) / (max_val - min_val);
    return static_cast<u32>(std::round(normalized * static_cast<f32>(max_int)));
}

f32 NetQuantizer::dequantize_float(u32 quantized, f32 min_val, f32 max_val, u32 bit_count) {
    if (bit_count == 0 || max_val <= min_val) {
        return min_val;
    }
    bit_count = std::min(bit_count, 32u);
    const u32 max_int = (bit_count == 32) ? 0xFFFFFFFFu : ((1u << bit_count) - 1u);

    const f32 normalized = static_cast<f32>(quantized) / static_cast<f32>(max_int);
    return min_val + normalized * (max_val - min_val);
}

void NetQuantizer::write_quantized_vec3(BitWriter& writer, const Vec3& v, const Vec3& min_bounds, const Vec3& max_bounds, u32 bits_per_axis) {
    const u32 qx = quantize_float(v.x, min_bounds.x, max_bounds.x, bits_per_axis);
    const u32 qy = quantize_float(v.y, min_bounds.y, max_bounds.y, bits_per_axis);
    const u32 qz = quantize_float(v.z, min_bounds.z, max_bounds.z, bits_per_axis);

    writer.write_bits(qx, bits_per_axis);
    writer.write_bits(qy, bits_per_axis);
    writer.write_bits(qz, bits_per_axis);
}

Vec3 NetQuantizer::read_quantized_vec3(BitReader& reader, const Vec3& min_bounds, const Vec3& max_bounds, u32 bits_per_axis) {
    const u32 qx = static_cast<u32>(reader.read_bits(bits_per_axis));
    const u32 qy = static_cast<u32>(reader.read_bits(bits_per_axis));
    const u32 qz = static_cast<u32>(reader.read_bits(bits_per_axis));

    return Vec3(
        dequantize_float(qx, min_bounds.x, max_bounds.x, bits_per_axis),
        dequantize_float(qy, min_bounds.y, max_bounds.y, bits_per_axis),
        dequantize_float(qz, min_bounds.z, max_bounds.z, bits_per_axis)
    );
}

void NetQuantizer::write_compressed_quat(BitWriter& writer, const Quat& q, u32 bits_per_component) {
    // 1. Normalize quaternion
    f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-6f) {
        len = 1.0f;
    }
    const f32 inv_len = 1.0f / len;
    f32 c[4] = { q.x * inv_len, q.y * inv_len, q.z * inv_len, q.w * inv_len };

    // 2. Find index with largest magnitude
    u32 max_idx = 0;
    f32 max_val = std::abs(c[0]);
    for (u32 i = 1; i < 4; ++i) {
        const f32 abs_val = std::abs(c[i]);
        if (abs_val > max_val) {
            max_val = abs_val;
            max_idx = i;
        }
    }

    // 3. Ensure largest component is positive (q and -q are identical 3D rotations)
    if (c[max_idx] < 0.0f) {
        for (u32 i = 0; i < 4; ++i) {
            c[i] = -c[i];
        }
    }

    // 4. Write largest component index in 2 bits
    writer.write_bits(max_idx, 2);

    // 5. Quantize the other 3 components in range [-INV_SQRT_TWO, +INV_SQRT_TWO]
    for (u32 i = 0; i < 4; ++i) {
        if (i != max_idx) {
            const u32 qval = quantize_float(c[i], -INV_SQRT_TWO, INV_SQRT_TWO, bits_per_component);
            writer.write_bits(qval, bits_per_component);
        }
    }
}

Quat NetQuantizer::read_compressed_quat(BitReader& reader, u32 bits_per_component) {
    const u32 max_idx = static_cast<u32>(reader.read_bits(2));

    f32 c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    f32 sum_sq = 0.0f;

    for (u32 i = 0; i < 4; ++i) {
        if (i != max_idx) {
            const u32 qval = static_cast<u32>(reader.read_bits(bits_per_component));
            c[i] = dequantize_float(qval, -INV_SQRT_TWO, INV_SQRT_TWO, bits_per_component);
            sum_sq += c[i] * c[i];
        }
    }

    // Reconstruct the missing largest component
    c[max_idx] = std::sqrt(std::max(0.0f, 1.0f - sum_sq));

    return Quat(c[0], c[1], c[2], c[3]);
}

} // namespace nf::net
