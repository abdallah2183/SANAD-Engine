// Tests/RHITests/test_gpu_timing.cpp — Phase 39 GPU timestamps
//
// These tests exercise the real Vulkan query path and the RenderGraph
// instrumentation without needing the Basic3D shader set:
//   - a raw timestamp pair brackets a known GPU transfer and must be readable
//     only after the submission fence signals;
//   - RenderGraph::execute brackets its passes with the caller's pool and
//     reports the right names/query indices;
//   - polling a query that was reset but never written reports "not ready"
//     rather than blocking or returning zeroes.
//
// Renderer3D's integration (frame + pass stats through its own pools) lives in
// test_3d_renderer.cpp because it needs the full Basic3D scene pipeline.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/RenderGraph.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace {

using namespace nf;
using namespace nf::test;
namespace rhi = nf::rhi;
namespace rendering = nf::rendering;

f64 delta_ms(u64 begin, u64 end, u32 valid_bits, f64 period_ns) {
    u64 delta = end - begin;
    const u32 bits = std::min<u32>(valid_bits, 64);
    if (bits < 64) delta &= (u64{1} << bits) - 1;
    return static_cast<f64>(delta) * period_ns / 1'000'000.0;
}

} // namespace

NF_TEST(timestamp_queries_measure_a_gpu_transfer) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (!dev.supports_timestamp_queries()) {
        NF_SKIP("timestamp queries unsupported by this device/queue");
    }
    NF_CHECK(dev.timestamp_valid_bits() > 0);
    NF_CHECK(dev.timestamp_period_ns() > 0.0);

    auto pool = dev.create_timestamp_query_pool(2);
    NF_CHECK(pool);
    NF_CHECK_EQ(pool->count(), 2u);

    constexpr usize kCopyBytes = 8 * 1024 * 1024;
    rhi::BufferDesc src_desc{};
    src_desc.size = kCopyBytes;
    src_desc.usage = rhi::BufferUsage::TransferSrc;
    src_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto src = dev.create_buffer(src_desc);
    NF_CHECK(src);

    rhi::BufferDesc dst_desc{};
    dst_desc.size = kCopyBytes;
    dst_desc.usage = rhi::BufferUsage::TransferDst;
    dst_desc.memory = rhi::MemoryUsage::GPUOnly;
    auto dst = dev.create_buffer(dst_desc);
    NF_CHECK(dst);

    std::vector<u8> fill(kCopyBytes, 0x5A);
    src->update(fill.data(), 0, fill.size());

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    NF_CHECK(cmd);
    NF_CHECK(fence);

    cmd->begin();
    cmd->reset_query_pool(*pool, 0, pool->count());
    cmd->write_timestamp(*pool, 0, rhi::PipelineStage::TopOfPipe);
    // Four back-to-back copies make the interval comfortably longer than the
    // timestamp tick on every real GPU, without relying on a sleep or a stall.
    for (u32 i = 0; i < 4; ++i) {
        cmd->copy_buffer(*src, *dst, 0, 0, kCopyBytes);
    }
    cmd->write_timestamp(*pool, 1, rhi::PipelineStage::BottomOfPipe);
    cmd->end();

    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));

    std::array<u64, 2> stamps{};
    NF_CHECK(pool->get_results(stamps));
    NF_CHECK(stamps[0] != stamps[1]); // real work advanced the GPU clock
    const f64 measured = delta_ms(stamps[0], stamps[1],
                                  dev.timestamp_valid_bits(), dev.timestamp_period_ns());
    NF_LOG_WARN(nf::LogCategory::RHI, "timestamp transfer: {:.4f} ms ({} -> {})",
                measured, stamps[0], stamps[1]);
    NF_CHECK(measured >= 0.0);
    NF_CHECK(measured < 1000.0);

    // Out-of-range requests are rejected instead of reading another pool's
    // memory or writing past the end of the caller's span.
    std::array<u64, 2> too_many{};
    NF_CHECK(!pool->get_results(too_many, 1));
    std::array<u64, 1> past_end{};
    NF_CHECK(!pool->get_results(past_end, 2));
}

NF_TEST(rendergraph_brackets_passes_with_timestamps) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (!dev.supports_timestamp_queries()) {
        NF_SKIP("timestamp queries unsupported by this device/queue");
    }

    // Query 0 is intentionally left unused: the test asks for a non-zero first
    // index so an off-by-one in the graph's indexing cannot pass by luck.
    constexpr u32 kFirstQuery = 1;
    auto pool = dev.create_timestamp_query_pool(7);
    NF_CHECK(pool);

    constexpr usize kCopyBytes = 1024 * 1024;
    auto make_stage = [&](rhi::Buffer& from, rhi::Buffer& to) {
        return [&from, &to](rhi::CommandBuffer& cmd) {
            cmd.copy_buffer(from, to, 0, 0, kCopyBytes);
        };
    };

    auto make_buffer = [&](rhi::BufferUsage usage, rhi::MemoryUsage memory) {
        rhi::BufferDesc desc{};
        desc.size = kCopyBytes;
        desc.usage = usage;
        desc.memory = memory;
        return dev.create_buffer(desc);
    };
    auto src_a = make_buffer(rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::CPUToGPU);
    auto dst_a = make_buffer(rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GPUOnly);
    auto src_b = make_buffer(rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::CPUToGPU);
    auto dst_b = make_buffer(rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GPUOnly);
    NF_CHECK(src_a && dst_a && src_b && dst_b);
    std::vector<u8> fill(kCopyBytes, 0x33);
    src_a->update(fill.data(), 0, fill.size());
    src_b->update(fill.data(), 0, fill.size());

    rendering::RenderGraph graph(dev);
    rendering::RGPassDesc a{};
    a.name = "CopyA";
    a.execute = make_stage(*src_a, *dst_a);
    graph.add_pass(a);
    rendering::RGPassDesc b{};
    b.name = "CopyB";
    b.execute = make_stage(*src_b, *dst_b);
    graph.add_pass(b);
    NF_CHECK(graph.compile());

    graph.set_pass_timing(pool.get(), kFirstQuery, 3);

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    NF_CHECK(cmd && fence);
    cmd->begin();
    cmd->reset_query_pool(*pool, 0, pool->count());
    graph.execute(*cmd);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));

    const auto& timings = graph.pass_timings();
    NF_CHECK_EQ(timings.size(), 2u);
    if (timings.size() == 2) {
        NF_CHECK_EQ(timings[0].name, std::string("CopyA"));
        NF_CHECK_EQ(timings[1].name, std::string("CopyB"));
        NF_CHECK_EQ(timings[0].begin_query, kFirstQuery);
        NF_CHECK_EQ(timings[0].end_query, kFirstQuery + 1);
        NF_CHECK_EQ(timings[1].begin_query, kFirstQuery + 2);
        NF_CHECK_EQ(timings[1].end_query, kFirstQuery + 3);
    }

    std::array<u64, 4> stamps{};
    NF_CHECK(pool->get_results(stamps, kFirstQuery));
    for (u32 pass = 0; pass < 2; ++pass) {
        const f64 ms = delta_ms(stamps[pass * 2], stamps[pass * 2 + 1],
                                dev.timestamp_valid_bits(), dev.timestamp_period_ns());
        NF_CHECK(ms >= 0.0);
        NF_CHECK(ms < 1000.0);
    }

    // Queries 5 and 6 were reset but never written. A non-blocking poll must
    // report "not ready" (availability 0), not block and not fabricate values.
    std::array<u64, 2> unwritten{};
    NF_CHECK(!pool->get_results(unwritten, 5));

    graph.reset(false);
    NF_CHECK(graph.pass_timings().empty());
}
