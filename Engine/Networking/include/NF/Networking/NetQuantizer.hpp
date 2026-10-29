#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Networking/BitStream.hpp>

namespace nf::net {

/// Network data compression and quantization helpers
class NetQuantizer {
public:
    /// Quantize a float in range [min_val, max_val] into an integer of bit_count bits
    static u32 quantize_float(f32 val, f32 min_val, f32 max_val, u32 bit_count);

    /// Dequantize an integer back to float in range [min_val, max_val]
    static f32 dequantize_float(u32 quantized, f32 min_val, f32 max_val, u32 bit_count);

    /// Write quantized 3D position vector
    static void write_quantized_vec3(BitWriter& writer, const Vec3& v, const Vec3& min_bounds, const Vec3& max_bounds, u32 bits_per_axis = 16);

    /// Read quantized 3D position vector
    static Vec3 read_quantized_vec3(BitReader& reader, const Vec3& min_bounds, const Vec3& max_bounds, u32 bits_per_axis = 16);

    /// Compress unit quaternion using "Smallest Three" algorithm (saves up to 75% bandwidth)
    static void write_compressed_quat(BitWriter& writer, const Quat& q, u32 bits_per_component = 10);

    /// Decompress unit quaternion from "Smallest Three"
    static Quat read_compressed_quat(BitReader& reader, u32 bits_per_component = 10);
};

} // namespace nf::net
