#include <NF/Test/TestFramework.hpp>
#include <NF/Networking/BitStream.hpp>
#include <NF/Networking/NetQuantizer.hpp>
#include <NF/Networking/RpcDispatcher.hpp>
#include <cmath>

using namespace nf;
using namespace nf::net;

NF_TEST(bitstream_arbitrary_bit_widths) {
    BitWriter writer;
    writer.write_bool(true);
    writer.write_bits(5, 3);        // 5 = 101 in 3 bits
    writer.write_bits(0x5A, 7);     // 7 bits
    writer.write_bits(4095, 12);    // 12 bits
    writer.write_bits(0x12345678, 32);

    BitReader reader(writer.buffer());
    NF_CHECK(reader.read_bool());
    NF_CHECK_EQ(reader.read_bits(3), 5u);
    NF_CHECK_EQ(reader.read_bits(7), 0x5Au);
    NF_CHECK_EQ(reader.read_bits(12), 4095u);
    NF_CHECK_EQ(reader.read_bits(32), 0x12345678u);
    NF_CHECK(!reader.is_overflow());
}

NF_TEST(bitstream_string_and_floats) {
    BitWriter writer;
    writer.write_string("SANAD Engine Netcode");
    writer.write_float(3.14159f);
    writer.write_i32(-1337);

    BitReader reader(writer.buffer());
    NF_CHECK_EQ(reader.read_string(), "SANAD Engine Netcode");
    NF_CHECK_NEAR(reader.read_float(), 3.14159f, 1e-5f);
    NF_CHECK_EQ(reader.read_i32(), -1337);
}

NF_TEST(quantizer_float_and_vec3) {
    const f32 min_val = -100.0f;
    const f32 max_val = 100.0f;
    const u32 bits = 16;

    f32 original = 42.123f;
    u32 quantized = NetQuantizer::quantize_float(original, min_val, max_val, bits);
    f32 decompressed = NetQuantizer::dequantize_float(quantized, min_val, max_val, bits);

    // Max error is (max - min) / 65535 = 200 / 65535 ~ 0.00305
    NF_CHECK_NEAR(decompressed, original, 0.005f);

    // Vec3 test
    Vec3 min_bounds(-500.0f, -100.0f, -500.0f);
    Vec3 max_bounds(500.0f, 100.0f, 500.0f);
    Vec3 original_pos(123.4f, 15.6f, -321.8f);

    BitWriter writer;
    NetQuantizer::write_quantized_vec3(writer, original_pos, min_bounds, max_bounds, 16);

    BitReader reader(writer.buffer());
    Vec3 decomp_pos = NetQuantizer::read_quantized_vec3(reader, min_bounds, max_bounds, 16);

    NF_CHECK_NEAR(decomp_pos.x, original_pos.x, 0.05f);
    NF_CHECK_NEAR(decomp_pos.y, original_pos.y, 0.05f);
    NF_CHECK_NEAR(decomp_pos.z, original_pos.z, 0.05f);
}

NF_TEST(quantizer_quaternion_smallest_three) {
    // Rotation 45 degrees around Y axis
    Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), 0.785398f);

    BitWriter writer;
    NetQuantizer::write_compressed_quat(writer, q, 10); // 2 bits index + 3x10 bits = 32 bits (4 bytes!)
    NF_CHECK_EQ(writer.bit_count(), 32u);

    BitReader reader(writer.buffer());
    Quat decomp_q = NetQuantizer::read_compressed_quat(reader, 10);

    // Dot product between unit quaternions: |q1 . q2| ~ 1.0
    f32 dot = q.x * decomp_q.x + q.y * decomp_q.y + q.z * decomp_q.z + q.w * decomp_q.w;
    NF_CHECK(std::abs(dot) > 0.999f);
}

NF_TEST(rpc_dispatcher_routing_and_payload) {
    RpcDispatcher dispatcher;

    bool chat_received = false;
    std::string chat_msg;
    u32 sender = 0;

    dispatcher.register_rpc(101, "ChatMessage", [&](u32 sender_id, BitReader& payload) {
        sender = sender_id;
        chat_msg = payload.read_string();
        chat_received = true;
    });

    // Write RPC payload
    BitWriter payload_writer;
    payload_writer.write_string("Hello World from Client 7");

    BitReader reader(payload_writer.buffer());
    bool ok = dispatcher.dispatch(101, 7, reader);

    NF_CHECK(ok);
    NF_CHECK(chat_received);
    NF_CHECK_EQ(sender, 7u);
    NF_CHECK_EQ(chat_msg, "Hello World from Client 7");
    NF_CHECK_EQ(dispatcher.total_dispatched_count(), 1u);

    // Unknown RPC returns false
    BitReader empty_reader(nullptr, 0);
    bool fail = dispatcher.dispatch(999, 1, empty_reader);
    NF_CHECK(!fail);
}
