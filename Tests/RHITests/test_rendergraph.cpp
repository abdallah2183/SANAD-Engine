// Tests/RHITests/test_rendergraph.cpp — proves RenderGraph orders Pass A → Pass B,
// inserts the TransferDst → ShaderRead barrier, and produces correct pixels.
//
// Pass A writes a 64x64 checker texture (upload)
// Pass B reads that texture and draws a quad to a 128x128 offscreen target
// The graph must infer the dependency (A before B) and transition the texture.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/PipelineCache.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

namespace {

using namespace nf;
using namespace nf::test;
using namespace nf::rendering;

#ifndef NF_RHI_TEST_QUAD_SHADER_DIR
    #define NF_RHI_TEST_QUAD_SHADER_DIR ""
#endif

struct Vertex { float x,y,u,v; };
constexpr std::array<Vertex,4> kVerts{{
    {-0.8f,-0.8f,0,0},{0.8f,-0.8f,1,0},{0.8f,0.8f,1,1},{-0.8f,0.8f,0,1}
}};
constexpr std::array<u32,6> kIdx{0,1,2,2,3,0};
constexpr u32 kTexSize=64;
constexpr std::array<Pixel,4> kColors{{ {255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255} }};

std::vector<u8> make_checker(){
    std::vector<u8> d(kTexSize*kTexSize*4);
    for(u32 y=0;y<kTexSize;++y) for(u32 x=0;x<kTexSize;++x){
        bool left=x<kTexSize/2; bool top=y<kTexSize/2;
        // Texel row 0 is the TOP row in Vulkan, so top=y<half is the upper half.
        usize q=0;
        if(top && left) q=0; else if(top) q=1; else if(left) q=2; else q=3;
        Pixel c=kColors[q];
        usize o=(static_cast<usize>(y)*kTexSize+x)*4;
        d[o+0]=c.r; d[o+1]=c.g; d[o+2]=c.b; d[o+3]=c.a;
    }
    return d;
}
bool near_pix(const Pixel& a, const Pixel& b, u8 tol=8){
    auto d=[tol](u8 x,u8 y){int diff=int(x)-int(y); return diff>=-int(tol)&&diff<=int(tol);};
    return d(a.r,b.r)&&d(a.g,b.g)&&d(a.b,b.b);
}

} // namespace

NF_TEST(rendergraph_orders_writes_before_reads_and_transitions) {
    const GpuFixture& f = require_gpu();
    auto& dev=*f.device;

    // Shaders
    auto shader_dir = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    NF_CHECK(!shader_dir.empty());
    auto vc=load_spirv(shader_dir/"textured_quad_vert.spv");
    auto fc=load_spirv(shader_dir/"textured_quad_frag.spv");
    NF_CHECK(!vc.empty() && !fc.empty());
    auto vs=dev.create_shader_module({vc, rhi::ShaderStage::Vertex});
    auto fs=dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    NF_CHECK(vs && fs);

    // Offscreen target 128x128 (will be graph resource via import)
    constexpr u32 W=128,H=128;
    rhi::TextureDesc target_desc{}; target_desc.width=W; target_desc.height=H;
    target_desc.format=rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage=rhi::ImageUsage::ColorAtt|rhi::ImageUsage::TransferSrc;
    auto target=dev.create_texture(target_desc); NF_CHECK(target);

    // RenderPass for the quad
    rhi::ColorAttachment ca{}; ca.format=target->format();
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rp_desc{}; rp_desc.color_attachments=std::span<const rhi::ColorAttachment>(atts);
    rp_desc.present_source=false;
    auto render_pass=dev.create_render_pass(rp_desc); NF_CHECK(render_pass);
    const std::array<rhi::Texture*,1> fb_tex{target.get()};
    auto framebuffer=dev.create_framebuffer(*render_pass, std::span<rhi::Texture* const>(fb_tex), nullptr);
    NF_CHECK(framebuffer);

    // Vertex/index buffers (GPUOnly)
    constexpr usize vb_size=sizeof(Vertex)*kVerts.size();
    rhi::BufferDesc vbd{}; vbd.size=vb_size; vbd.usage=rhi::BufferUsage::Vertex|rhi::BufferUsage::TransferDst; vbd.memory=rhi::MemoryUsage::GPUOnly;
    auto vb=dev.create_buffer(vbd); NF_CHECK(vb); vb->update(kVerts.data(),0,vb_size);
    constexpr usize ib_size=sizeof(u32)*kIdx.size();
    rhi::BufferDesc ibd{}; ibd.size=ib_size; ibd.usage=rhi::BufferUsage::Index|rhi::BufferUsage::TransferDst; ibd.memory=rhi::MemoryUsage::GPUOnly;
    auto ib=dev.create_buffer(ibd); NF_CHECK(ib); ib->update(kIdx.data(),0,ib_size);

    // Staging for texture upload (CPUToGPU)
    auto tex_data=make_checker();
    rhi::BufferDesc sdesc{}; sdesc.size=tex_data.size(); sdesc.usage=rhi::BufferUsage::TransferSrc; sdesc.memory=rhi::MemoryUsage::CPUToGPU;
    auto staging=dev.create_buffer(sdesc); NF_CHECK(staging);
    void* m=staging->map(); NF_CHECK(m); std::memcpy(m,tex_data.data(),tex_data.size()); staging->unmap();

    // Sampler + Descriptor via Material + Allocator
    rhi::SamplerDesc sdesc2{}; sdesc2.mag=rhi::Filter::Nearest; sdesc2.min=rhi::Filter::Nearest;
    sdesc2.address_u=rhi::AddressMode::ClampToEdge; sdesc2.address_v=rhi::AddressMode::ClampToEdge;
    auto sampler=dev.create_sampler(sdesc2); NF_CHECK(sampler);

    const std::array<rhi::DescriptorBinding,1> binds{{{0,rhi::DescriptorType::SampledImage,rhi::ShaderStage::Fragment,1}}};
    rhi::DescriptorSetLayoutDesc ld{}; ld.bindings=std::span<const rhi::DescriptorBinding>(binds);
    auto set_layout=dev.create_descriptor_set_layout(ld); NF_CHECK(set_layout);

    // Material + PipelineCache
    PipelineCache pcache(dev);
    rhi::VertexLayout vl{}; vl.binding=0; vl.stride=sizeof(Vertex);
    const std::array<rhi::VertexAttrib,2> va{{ {0, offsetof(Vertex,x), rhi::Format::R32G32_SFloat},{1, offsetof(Vertex,u), rhi::Format::R32G32_SFloat} }};
    vl.attributes=std::span<const rhi::VertexAttrib>(va);
    MaterialDesc mdesc{};
    mdesc.vs=vs.get(); mdesc.fs=fs.get(); mdesc.render_pass=render_pass.get();
    mdesc.descriptor_set_layout=set_layout.get(); mdesc.vertex_layout=vl;
    mdesc.topology=rhi::PrimitiveTopology::TriangleList;
    mdesc.rasterizer.cull_mode=rhi::CullMode::None;
    Material material(dev, pcache, mdesc); NF_CHECK(material.valid());

    // Descriptor allocator (per-frame)
    auto allocator=dev.create_descriptor_allocator(16); NF_CHECK(allocator);
    MaterialInstance matInst(material, *allocator);
    // We need a texture view for the checker texture, but the texture itself is a graph resource.
    // For now create the graph texture as owned resource, then after compile get its rhi::Texture*
    // to create a view. To break the cycle, we create the material instance after the graph's
    // texture is available (inside Pass B's execute we will need the view). Instead we will
    // create the view on the fly in Pass B.

    // Readback buffer
    rhi::BufferDesc rb_desc{}; rb_desc.size=static_cast<usize>(W)*H*4; rb_desc.usage=rhi::BufferUsage::TransferDst; rb_desc.memory=rhi::MemoryUsage::GPUToCPU;
    auto readback=dev.create_buffer(rb_desc); NF_CHECK(readback);

    // ---- Build RenderGraph ----
    RenderGraph graph(dev);
    // Create checker texture as graph-owned resource
    rhi::TextureDesc tex_desc{}; tex_desc.width=kTexSize; tex_desc.height=kTexSize;
    tex_desc.format=rhi::Format::R8G8B8A8_UNorm; tex_desc.usage=rhi::ImageUsage::Sampled|rhi::ImageUsage::TransferDst;
    auto rg_tex = graph.create_texture("Checker", tex_desc);
    // Import the offscreen target (we already created it outside)
    auto rg_target = graph.import_texture("OffscreenTarget", target.get());

    // Pass A: upload — writes the checker texture
    RGPassDesc passA{};
    passA.name="UploadChecker";
    passA.writes={rg_tex};
    passA.execute=[&](rhi::CommandBuffer& cmd){
        rhi::Texture* tex = graph.get_texture(rg_tex);
        NF_ASSERT(tex != nullptr, "graph texture not found");
        cmd.copy_buffer_to_texture(*staging, *tex, 0, 0,0,kTexSize,kTexSize);
        // Leave in TRANSFER_DST; the graph will transition before the next pass's read
    };

    // Pass B: draw quad sampling the checker — reads rg_tex, writes rg_target
    RGPassDesc passB{};
    passB.name="DrawQuad";
    passB.reads={rg_tex};
    passB.writes={rg_target};
    passB.color_attachments={rg_target};
    passB.execute=[&](rhi::CommandBuffer& cmd){
        // The view and descriptor must already be prepared before execution
        const std::array<rhi::ClearValue,1> clears{ rhi::ClearValue{0,0,0,1} };
        cmd.begin_render_pass(*render_pass, *framebuffer, std::span<const rhi::ClearValue>(clears));
        matInst.bind_pipeline_and_descriptors(cmd);
        const std::array<const rhi::Buffer*,1> vbs{vb.get()};
        cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
        cmd.bind_index_buffer(*ib,0);
        cmd.set_viewport(0,0,W,H);
        cmd.set_scissor(0,0,W,H);
        cmd.draw_indexed(static_cast<u32>(kIdx.size()));
        cmd.end_render_pass();
    };

    // Add passes out of order to prove the graph reorders based on dependency
    // (Add B first, then A - the graph should still execute A before B)
    graph.add_pass(passB);
    graph.add_pass(passA);

    NF_CHECK(graph.compile());
    NF_CHECK(graph.pass_count()==2);

    // The checker texture was created on compile; now create a view for it and
    // wire the material. This keeps the view alive for the duration of the test.
    rhi::Texture* graph_tex = graph.get_texture(rg_tex);
    NF_CHECK(graph_tex != nullptr);
    rhi::TextureViewDesc view_desc{};
    view_desc.texture = graph_tex;
    view_desc.dimension = rhi::ViewDimension::View2D;
    view_desc.aspect = rhi::ImageAspect::Color;
    view_desc.base_mip = 0; view_desc.mip_count = 1;
    view_desc.base_layer = 0; view_desc.layer_count = 1;
    auto checker_view = dev.create_texture_view(view_desc);
    NF_CHECK(checker_view != nullptr);
    matInst.set_texture(0, *checker_view, *sampler);
    matInst.update();
    // Verify that execution order is A then B despite insertion order
    {
        const auto& order = graph.execution_order();
        NF_CHECK(order.size()==2);
        // The names in order should be UploadChecker first
        // Since we added B(0) then A(1), but A writes and B reads, order must be [1,0]
        // We can't directly check names without exposing, but we can check that the graph
        // actually executed correctly via pixels.
    }

    // Record and execute
    auto cmd=dev.create_command_buffer(); auto fence=dev.create_fence(false);
    NF_CHECK(cmd && fence);
    cmd->begin();
    graph.execute(*cmd);
    // After graph, copy target to readback (outside graph, as a final transfer)
    cmd->copy_texture_to_buffer(*target, *readback, 0,0,W,H,0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence=fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    dev.wait_idle();

    // Verify pixels
    Pixel* px = static_cast<Pixel*>(readback->map()); NF_CHECK(px);
    auto at=[&](u32 x,u32 y){ return px[static_cast<usize>(y)*W+x]; };
    Pixel tl=at(40,30), tr=at(88,30), bl=at(40,98), br=at(88,98), outside=at(2,2);
    // Keep for log
    NF_LOG_WARN(LogCategory::RHI, "Graph quad: TL({},{},{}) TR({},{},{}) BL({},{},{}) BR({},{},{})",
                tl.r,tl.g,tl.b, tr.r,tr.g,tr.b, bl.r,bl.g,bl.b, br.r,br.g,br.b);
    readback->unmap();

    NF_CHECK_EQ(outside.r, 0); NF_CHECK_EQ(outside.g,0); NF_CHECK_EQ(outside.b,0);
    NF_CHECK(near_pix(tl, kColors[0]));
    NF_CHECK(near_pix(tr, kColors[1]));
    NF_CHECK(near_pix(bl, kColors[2]));
    NF_CHECK(near_pix(br, kColors[3]));
}
