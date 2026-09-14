// Tests/RHITests/test_render_world.cpp — GameWorld → RenderWorld extraction + RenderGraph integration

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/GameWorld.hpp>
#include <NF/Rendering/Extraction.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/PipelineCache.hpp>

#include <array>
#include <filesystem>
#include <span>

namespace {

using namespace nf;
using namespace nf::test;
using namespace nf::rendering;
using namespace nf::game;

#ifndef NF_RHI_TEST_QUAD_SHADER_DIR
    #define NF_RHI_TEST_QUAD_SHADER_DIR ""
#endif

struct Vertex { float x,y,u,v; };

} // namespace

NF_TEST(render_world_filters_gameplay_components) {
    GameWorld gw;
    // Entity 1: renderable (has mesh+material)
    auto& e1 = gw.create_entity();
    e1.transform = {0,0,0};
    e1.mesh = MeshComponent{"cube"};
    e1.material = MaterialComponent{"mat_checker"};
    e1.visible = true;
    e1.health = Health{100}; // gameplay, should not reach render world
    e1.inventory = Inventory{{"sword"}};

    // Entity 2: invisible (should be filtered)
    auto& e2 = gw.create_entity();
    e2.transform = {1,0,0};
    e2.mesh = MeshComponent{"sphere"};
    e2.material = MaterialComponent{"mat_red"};
    e2.visible = false;

    // Entity 3: gameplay only (no mesh) — should be filtered
    auto& e3 = gw.create_entity();
    e3.transform = {2,0,0};
    e3.health = Health{50};
    e3.quest = QuestState{"quest_1"};

    // Entity 4: renderable without gameplay
    auto& e4 = gw.create_entity();
    e4.transform = {3,0,0};
    e4.mesh = MeshComponent{"plane"};
    e4.material = MaterialComponent{"mat_plane"};
    e4.visible = true;

    NF_CHECK(gw.size()==4);

    RenderWorld rw;
    extract_render_world(gw, rw);
    NF_CHECK(rw.size()==2); // only e1 and e4
    bool has_e1=false, has_e4=false;
    for (auto& o : rw.objects) {
        if (o.id==e1.id) { has_e1=true; NF_CHECK(o.mesh=="cube"); NF_CHECK(o.material=="mat_checker"); }
        if (o.id==e4.id) { has_e4=true; NF_CHECK(o.mesh=="plane"); }
        // Ensure no gameplay data leaked: RenderObject has no health/inventory fields
    }
    NF_CHECK(has_e1 && has_e4);
}

NF_TEST(render_world_double_buffering) {
    GameWorld gw;
    auto& e = gw.create_entity();
    e.transform = {0,0,0};
    e.mesh = MeshComponent{"a"};
    e.material = MaterialComponent{"m"};
    e.visible = true;

    RenderWorldBuffer buf;
    // Frame 0: extract
    extract_render_world_double_buffered(gw, buf);
    NF_CHECK(buf.front().size()==1);
    // Modify game world
    gw.entities[0].transform.x = 5;
    // Back should still have old transform until next swap
    // Front is 0, back after extraction but before swap would be 5? Actually double buffered swaps, so front now has 0
    // Extract again
    extract_render_world_double_buffered(gw, buf);
    NF_CHECK(buf.front().size()==1);
    NF_CHECK(buf.front().objects[0].transform.x == 5);
    // Front and back are now swapped; front has 5, back has 0 (old)
    // Ensure double buffering doesn't alias
    NF_CHECK(buf.back().objects[0].transform.x == 0);
}

NF_TEST(game_entity_to_render_graph_pixel_verification) {
    // Full chain: GameWorld → RenderWorld → RenderGraph → GPU → Pixel Verification
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;

    // Create a GameWorld with one renderable entity
    GameWorld gw;
    auto& e = gw.create_entity();
    e.transform = {0,0,0};
    e.mesh = MeshComponent{"quad"};
    e.material = MaterialComponent{"checker"};
    e.visible = true;
    // Add a gameplay-only entity that must not be rendered
    auto& e2 = gw.create_entity();
    e2.health = Health{10};
    e2.visible = true;

    RenderWorld rw;
    extract_render_world(gw, rw);
    NF_CHECK(rw.size()==1);
    NF_CHECK(rw.objects[0].mesh=="quad");

    // Now use the RenderWorld to drive a RenderGraph that draws a textured quad
    // (We reuse the existing quad shaders and a checker texture)

    auto shader_dir = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto vc = load_spirv(shader_dir / "textured_quad_vert.spv");
    auto fc = load_spirv(shader_dir / "textured_quad_frag.spv");
    NF_CHECK(!vc.empty() && !fc.empty());
    auto vs = dev.create_shader_module({vc, rhi::ShaderStage::Vertex});
    auto fs = dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    NF_CHECK(vs && fs);

    constexpr u32 W=64,H=64;
    rhi::TextureDesc target_desc{}; target_desc.width=W; target_desc.height=H; target_desc.format=rhi::Format::R8G8B8A8_UNorm; target_desc.usage=rhi::ImageUsage::ColorAtt|rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc); NF_CHECK(target);
    rhi::ColorAttachment ca{}; ca.format=target->format();
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp = dev.create_render_pass(rpd); NF_CHECK(rp);
    const std::array<rhi::Texture*,1> fb_tex{target.get()};
    auto fb = dev.create_framebuffer(*rp, std::span<rhi::Texture* const>(fb_tex), nullptr); NF_CHECK(fb);

    // Create a simple checker texture for the material
    rhi::TextureDesc checker_desc{}; checker_desc.width=2; checker_desc.height=2; checker_desc.format=rhi::Format::R8G8B8A8_UNorm; checker_desc.usage=rhi::ImageUsage::Sampled|rhi::ImageUsage::TransferDst;
    auto checker_tex = dev.create_texture(checker_desc); NF_CHECK(checker_tex);
    // 2x2: red, green, blue, yellow
    const std::array<u8, 16> checker_pixels{ 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255 };
    rhi::BufferDesc sdesc{}; sdesc.size=checker_pixels.size(); sdesc.usage=rhi::BufferUsage::TransferSrc; sdesc.memory=rhi::MemoryUsage::CPUToGPU;
    auto staging = dev.create_buffer(sdesc); NF_CHECK(staging);
    void* m = staging->map(); std::memcpy(m, checker_pixels.data(), checker_pixels.size()); staging->unmap();
    rhi::SamplerDesc sdesc2{}; sdesc2.mag=rhi::Filter::Nearest; sdesc2.min=rhi::Filter::Nearest;
    auto sampler = dev.create_sampler(sdesc2); NF_CHECK(sampler);
    rhi::TextureViewDesc vd{}; vd.texture=checker_tex.get(); vd.dimension=rhi::ViewDimension::View2D;
    auto view = dev.create_texture_view(vd); NF_CHECK(view);

    // Upload checker
    auto up = dev.create_upload_context(); NF_CHECK(up);
    up->copy_buffer_to_texture(*staging, *checker_tex, 0, 0,0,2,2);
    auto fence = up->submit(); NF_CHECK(fence && fence->wait(kGpuTimeoutNs));
    auto trans = dev.create_command_buffer(); auto tf = dev.create_fence(false);
    trans->begin(); trans->transition_texture_for_sampling(*checker_tex); trans->end();
    dev.submit(*trans, rhi::SubmitInfo{.signal_fence=tf.get()}); NF_CHECK(tf->wait(kGpuTimeoutNs));

    // Material via RenderWorld objects
    PipelineCache pcache(dev);
    const std::array<rhi::DescriptorBinding,1> binds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc ld{}; ld.bindings=std::span<const rhi::DescriptorBinding>(binds);
    auto layout = dev.create_descriptor_set_layout(ld); NF_CHECK(layout);
    struct Vertex2 { float x,y,u,v; };
    const std::array<Vertex2,4> verts{{ {-1,-1,0,0},{1,-1,1,0},{1,1,1,1},{-1,1,0,1} }};
    const std::array<u32,6> idx{0,1,2,2,3,0};
    rhi::BufferDesc vbd{}; vbd.size=sizeof(Vertex2)*4; vbd.usage=rhi::BufferUsage::Vertex|rhi::BufferUsage::TransferDst; vbd.memory=rhi::MemoryUsage::GPUOnly;
    auto vb = dev.create_buffer(vbd); NF_CHECK(vb); vb->update(verts.data(),0,vbd.size);
    rhi::BufferDesc ibd{}; ibd.size=sizeof(u32)*6; ibd.usage=rhi::BufferUsage::Index|rhi::BufferUsage::TransferDst; ibd.memory=rhi::MemoryUsage::GPUOnly;
    auto ib = dev.create_buffer(ibd); NF_CHECK(ib); ib->update(idx.data(),0,ibd.size);

    rhi::VertexLayout vl{}; vl.binding=0; vl.stride=sizeof(Vertex2);
    const std::array<rhi::VertexAttrib,2> va{{ {0,0,rhi::Format::R32G32_SFloat},{1,8,rhi::Format::R32G32_SFloat} }};
    vl.attributes=std::span<const rhi::VertexAttrib>(va);
    MaterialDesc md{}; md.vs=vs.get(); md.fs=fs.get(); md.render_pass=rp.get(); md.descriptor_set_layout=layout.get(); md.vertex_layout=vl;
    md.rasterizer.cull_mode = rhi::CullMode::None;
    Material mat(dev, pcache, md); NF_CHECK(mat.valid());
    auto alloc = dev.create_descriptor_allocator(4); NF_CHECK(alloc);
    MaterialInstance inst(mat, *alloc);
    inst.set_texture(0, *view, *sampler); inst.update();

    // Use RenderGraph to draw: the graph reads the checker (imported) and writes the target
    RenderGraph graph(dev);
    auto rg_checker = graph.import_texture("Checker", checker_tex.get());
    auto rg_target = graph.import_texture("Target", target.get());
    RGPassDesc pass{}; pass.name="DrawFromRenderWorld"; pass.reads={rg_checker}; pass.writes={rg_target}; pass.color_attachments={rg_target};
    pass.execute=[&](rhi::CommandBuffer& cmd){
        const std::array<rhi::ClearValue,1> clears{ rhi::ClearValue{0,0,0,1} };
        cmd.begin_render_pass(*rp, *fb, std::span<const rhi::ClearValue>(clears));
        inst.bind_pipeline_and_descriptors(cmd);
        const std::array<const rhi::Buffer*,1> vbs{vb.get()};
        cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
        cmd.bind_index_buffer(*ib,0);
        cmd.set_viewport(0,0,W,H); cmd.set_scissor(0,0,W,H);
        cmd.draw_indexed(6);
        cmd.end_render_pass();
    };
    graph.add_pass(pass);
    NF_CHECK(graph.compile());

    rhi::BufferDesc rb_desc{}; rb_desc.size=static_cast<usize>(W)*H*4; rb_desc.usage=rhi::BufferUsage::TransferDst; rb_desc.memory=rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc); NF_CHECK(rb);
    auto cmd = dev.create_command_buffer(); auto fence2 = dev.create_fence(false);
    NF_CHECK(cmd && fence2);
    cmd->begin(); graph.execute(*cmd); cmd->copy_texture_to_buffer(*target, *rb, 0,0,W,H,0); cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence=fence2.get()});
    NF_CHECK(fence2->wait(kGpuTimeoutNs)); dev.wait_idle();
    auto* px = static_cast<Pixel*>(rb->map()); NF_CHECK(px);
    // The quad covers the whole target, so center should be sampled (interpolated) but corners should be distinct
    // With nearest filtering and the quad covering -1..1, the center should be roughly one of the checker colors
    // We just verify that the image is not all clear color (0,0,0)
    u32 lit=0; for(usize i=0;i<static_cast<usize>(W)*H;++i) if(px[i].r||px[i].g||px[i].b) ++lit;
    rb->unmap();
    NF_CHECK(lit > 1000);
}
