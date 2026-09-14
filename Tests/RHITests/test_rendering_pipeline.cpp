// Tests/RHITests/test_rendering_pipeline.cpp — Pipeline Cache + Shader Hot Reload + Reflection tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Assert.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/ShaderReflection.hpp>
#include <NF/Rendering/ShaderManager.hpp>
#include <NF/Rendering/Material.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>
#include <vector>

#include <NF/Jobs/JobSystem.hpp>

namespace {

using namespace nf;
using namespace nf::test;
using namespace nf::rendering;

#ifndef NF_RHI_TEST_QUAD_SHADER_DIR
    #define NF_RHI_TEST_QUAD_SHADER_DIR ""
#endif

#ifndef NF_RHI_PUSH_SHADER_DIR
    #define NF_RHI_PUSH_SHADER_DIR ""
#endif

} // namespace

NF_TEST(pipeline_key_owns_layout_data) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;

    // Create a render pass for the pipeline
    rhi::TextureDesc td{}; td.width=16; td.height=16; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt;
    auto tex = dev.create_texture(td); NF_CHECK(tex);
    rhi::ColorAttachment ca{}; ca.format=td.format;
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp = dev.create_render_pass(rpd); NF_CHECK(rp);

    auto shader_dir = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto vc = load_spirv(shader_dir / "textured_quad_vert.spv");
    auto fc = load_spirv(shader_dir / "textured_quad_frag.spv");
    NF_CHECK(!vc.empty() && !fc.empty());
    auto vs = dev.create_shader_module({vc, rhi::ShaderStage::Vertex});
    auto fs = dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    NF_CHECK(vs && fs);

    const std::array<rhi::DescriptorBinding, 1> qbinds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc qld{}; qld.bindings = std::span<const rhi::DescriptorBinding>(qbinds);
    auto set_layout = dev.create_descriptor_set_layout(qld); NF_CHECK(set_layout);

    PipelineCache cache(dev);

    // The critical test: vertex layout attributes come from a temporary vector that dies immediately
    rhi::Pipeline* cached_pipeline = nullptr;
    {
        std::vector<rhi::VertexAttrib> temp_attribs;
        temp_attribs.push_back({0, 0, rhi::Format::R32G32_SFloat});
        temp_attribs.push_back({1, 8, rhi::Format::R32G32_SFloat});
        rhi::VertexLayout vl{}; vl.binding=0; vl.stride=16; vl.attributes=std::span<const rhi::VertexAttrib>(temp_attribs);
        rhi::PipelineDesc desc{};
        desc.vs=vs.get(); desc.fs=fs.get(); desc.render_pass=rp.get();
        desc.descriptor_set_layout=set_layout.get();
        desc.vertex_layout=vl;
        // temp_attribs will die at the end of this scope, but the cache key must remain valid
        cached_pipeline = cache.get_or_create(desc);
        NF_CHECK(cached_pipeline != nullptr);
        NF_CHECK(cache.size()==1);
        // temp_attribs dies here
    }

    // Now create an identical desc with a new temporary vector — it should hit the cache (same pipeline pointer)
    {
        std::vector<rhi::VertexAttrib> temp2;
        temp2.push_back({0, 0, rhi::Format::R32G32_SFloat});
        temp2.push_back({1, 8, rhi::Format::R32G32_SFloat});
        rhi::VertexLayout vl2{}; vl2.binding=0; vl2.stride=16; vl2.attributes=std::span<const rhi::VertexAttrib>(temp2);
        rhi::PipelineDesc desc2{};
        desc2.vs=vs.get(); desc2.fs=fs.get(); desc2.render_pass=rp.get();
        desc2.descriptor_set_layout=set_layout.get();
        desc2.vertex_layout=vl2;
        rhi::Pipeline* p2 = cache.get_or_create(desc2);
        NF_CHECK(p2 == cached_pipeline); // must be same object (cache hit) even though source vectors were temporaries
    }

    // Different layout must create a new pipeline
    {
        std::vector<rhi::VertexAttrib> temp3;
        temp3.push_back({0, 0, rhi::Format::R32G32_SFloat});
        temp3.push_back({1, 12, rhi::Format::R32G32_SFloat}); // different layout
        rhi::VertexLayout vl3{}; vl3.binding=0; vl3.stride=20; vl3.attributes=std::span<const rhi::VertexAttrib>(temp3);
        rhi::PipelineDesc desc3{};
        desc3.vs=vs.get(); desc3.fs=fs.get(); desc3.render_pass=rp.get();
        desc3.descriptor_set_layout=set_layout.get();
        desc3.vertex_layout=vl3;
        rhi::Pipeline* p3 = cache.get_or_create(desc3);
        NF_CHECK(p3 != nullptr);
        NF_CHECK(p3 != cached_pipeline);
        NF_CHECK(cache.size()==2);
    }
}

NF_TEST(pipeline_cache_reuses_identical_pipeline) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    rhi::TextureDesc td{}; td.width=8; td.height=8; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt;
    auto tex=dev.create_texture(td); NF_CHECK(tex);
    rhi::ColorAttachment ca{}; ca.format=td.format;
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp=dev.create_render_pass(rpd); NF_CHECK(rp);
    auto sd = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto vc=load_spirv(sd/"textured_quad_vert.spv"); auto fc=load_spirv(sd/"textured_quad_frag.spv");
    auto vs=dev.create_shader_module({vc, rhi::ShaderStage::Vertex}); auto fs=dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    const std::array<rhi::DescriptorBinding,1> qbinds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc qld{}; qld.bindings = std::span<const rhi::DescriptorBinding>(qbinds);
    auto set_layout = dev.create_descriptor_set_layout(qld); NF_CHECK(set_layout);

    const std::array<rhi::VertexAttrib,2> qattribs{{
        {0, 0, rhi::Format::R32G32_SFloat},
        {1, 8, rhi::Format::R32G32_SFloat}
    }};
    rhi::VertexLayout qvl{}; qvl.binding=0; qvl.stride=16; qvl.attributes=std::span<const rhi::VertexAttrib>(qattribs);

    PipelineCache cache(dev);
    rhi::PipelineDesc desc{}; desc.vs=vs.get(); desc.fs=fs.get(); desc.render_pass=rp.get();
    desc.descriptor_set_layout=set_layout.get();
    desc.vertex_layout=qvl;
    rhi::Pipeline* p1 = cache.get_or_create(desc);
    rhi::Pipeline* p2 = cache.get_or_create(desc);
    NF_CHECK(p1 && p2);
    NF_CHECK(p1 == p2);
    NF_CHECK(cache.size()==1);
}

NF_TEST(pipeline_cache_separates_different_keys) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    rhi::TextureDesc td{}; td.width=8; td.height=8; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt;
    auto tex=dev.create_texture(td);
    rhi::ColorAttachment ca{}; ca.format=td.format;
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp=dev.create_render_pass(rpd);
    auto sd = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto vc=load_spirv(sd/"textured_quad_vert.spv"); auto fc=load_spirv(sd/"textured_quad_frag.spv");
    auto vs=dev.create_shader_module({vc, rhi::ShaderStage::Vertex}); auto fs=dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    // Create a second fragment shader with same code but different module object
    auto fs2=dev.create_shader_module({fc, rhi::ShaderStage::Fragment});

    const std::array<rhi::DescriptorBinding,1> qbinds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc qld{}; qld.bindings = std::span<const rhi::DescriptorBinding>(qbinds);
    auto set_layout = dev.create_descriptor_set_layout(qld); NF_CHECK(set_layout);

    const std::array<rhi::VertexAttrib,2> qattribs{{
        {0, 0, rhi::Format::R32G32_SFloat},
        {1, 8, rhi::Format::R32G32_SFloat}
    }};
    rhi::VertexLayout qvl{}; qvl.binding=0; qvl.stride=16; qvl.attributes=std::span<const rhi::VertexAttrib>(qattribs);

    PipelineCache cache(dev);
    rhi::PipelineDesc d1{}; d1.vs=vs.get(); d1.fs=fs.get(); d1.render_pass=rp.get();
    d1.descriptor_set_layout=set_layout.get(); d1.vertex_layout=qvl;
    rhi::PipelineDesc d2{}; d2.vs=vs.get(); d2.fs=fs2.get(); d2.render_pass=rp.get();
    d2.descriptor_set_layout=set_layout.get(); d2.vertex_layout=qvl;
    rhi::Pipeline* p1 = cache.get_or_create(d1);
    rhi::Pipeline* p2 = cache.get_or_create(d2);
    NF_CHECK(p1 && p2);
    NF_CHECK(p1 != p2);
    NF_CHECK(cache.size()==2);
}

NF_TEST(shader_reflection_matches_descriptor_layout) {
    require_gpu();
    // Use the textured quad fragment shader which has one CombinedImageSampler at set 0 binding 0
    auto sd = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto fc = load_spirv(sd / "textured_quad_frag.spv");
    NF_CHECK(!fc.empty());
    auto refl = reflect_spirv(std::span<const u8>(fc), rhi::ShaderStage::Fragment);
    NF_CHECK(refl.valid);
    NF_CHECK(refl.bindings.size()==1);
    const auto& b = refl.bindings[0];
    NF_CHECK_EQ(b.set, 0u);
    NF_CHECK_EQ(b.binding, 0u);
    NF_CHECK(b.type == rhi::DescriptorType::SampledImage);
    NF_CHECK(b.stage == rhi::ShaderStage::Fragment);

    // The reflected bindings should be convertible to a DescriptorSetLayout that matches the manual one
    auto bindings = reflection_to_descriptor_bindings(refl);
    NF_CHECK(bindings.size()==1);
    NF_CHECK_EQ(bindings[0].binding, 0u);
    NF_CHECK(bindings[0].type == rhi::DescriptorType::SampledImage);

    // Vertex shader reflection should have 2 vertex inputs at locations 0 and 1
    auto vc = load_spirv(sd / "textured_quad_vert.spv");
    auto vrefl = reflect_spirv(std::span<const u8>(vc), rhi::ShaderStage::Vertex);
    NF_CHECK(vrefl.valid);
    NF_CHECK(vrefl.vertex_inputs.size()==2);
    bool has_loc0=false, has_loc1=false;
    for (auto& vi : vrefl.vertex_inputs) {
        if (vi.location==0) has_loc0=true;
        if (vi.location==1) has_loc1=true;
        NF_CHECK(vi.format == rhi::Format::R32G32_SFloat);
    }
    NF_CHECK(has_loc0 && has_loc1);

    // Push constant shader reflection
    auto push_dir = std::filesystem::path(NF_RHI_PUSH_SHADER_DIR);
    auto pfc = load_spirv(push_dir / "push_constant_frag.spv");
    if (!pfc.empty()) {
        auto prefl = reflect_spirv(std::span<const u8>(pfc), rhi::ShaderStage::Fragment);
        NF_CHECK(prefl.valid);
        NF_CHECK(prefl.push_constants.size()==1);
        NF_CHECK(prefl.push_constants[0].size==16);
    }
}

NF_TEST(shader_change_invalidates_dependents) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    rhi::TextureDesc td{}; td.width=8; td.height=8; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt;
    auto tex=dev.create_texture(td);
    rhi::ColorAttachment ca{}; ca.format=td.format;
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp=dev.create_render_pass(rpd);
    auto sd = std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR);
    auto vc=load_spirv(sd/"textured_quad_vert.spv"); auto fc=load_spirv(sd/"textured_quad_frag.spv");
    auto vs=dev.create_shader_module({vc, rhi::ShaderStage::Vertex});
    auto fs=dev.create_shader_module({fc, rhi::ShaderStage::Fragment});
    auto fs2=dev.create_shader_module({fc, rhi::ShaderStage::Fragment}); // second instance, same code but different object

    const std::array<rhi::DescriptorBinding,1> qbinds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc qld{}; qld.bindings = std::span<const rhi::DescriptorBinding>(qbinds);
    auto set_layout = dev.create_descriptor_set_layout(qld); NF_CHECK(set_layout);

    const std::array<rhi::VertexAttrib,2> qattribs{{
        {0, 0, rhi::Format::R32G32_SFloat},
        {1, 8, rhi::Format::R32G32_SFloat}
    }};
    rhi::VertexLayout qvl{}; qvl.binding=0; qvl.stride=16; qvl.attributes=std::span<const rhi::VertexAttrib>(qattribs);

    PipelineCache cache(dev);
    rhi::PipelineDesc d1{}; d1.vs=vs.get(); d1.fs=fs.get(); d1.render_pass=rp.get();
    d1.descriptor_set_layout=set_layout.get(); d1.vertex_layout=qvl;
    rhi::PipelineDesc d2{}; d2.vs=vs.get(); d2.fs=fs2.get(); d2.render_pass=rp.get();
    d2.descriptor_set_layout=set_layout.get(); d2.vertex_layout=qvl;
    rhi::Pipeline* p1 = cache.get_or_create(d1);
    rhi::Pipeline* p2 = cache.get_or_create(d2);
    NF_CHECK(p1 != nullptr && p2 != nullptr);
    NF_CHECK(cache.size()==2);

    // Invalidate fs (used by p1) — only p1 should be removed, p2 stays
    size_t removed = cache.invalidate_shader(fs.get());
    NF_CHECK_EQ(removed, 1u);
    NF_CHECK(cache.size()==1);
    // p2 should still be cached
    rhi::Pipeline* p2_again = cache.get_or_create(d2);
    NF_CHECK(p2_again == p2);
    // p1 should be recreated (new object)
    rhi::Pipeline* p1_new = cache.get_or_create(d1);
    NF_CHECK(p1_new != nullptr);
    NF_CHECK(cache.size() == 2);
    // NOTE: p1_new != p1 is intentionally NOT asserted — p1 was freed by the
    // invalidation above, and the allocator may hand the same address back
    // for p1_new (ABA), making a dangling-pointer comparison meaningless.
}

NF_TEST(shader_compile_failure_keeps_previous_version) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;

    // Create a temp shader file with valid GLSL
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "nf_test_shader.frag";
    {
        std::ofstream out(tmp);
        out << "#version 450\nlayout(location=0) out vec4 c; void main(){ c = vec4(1,0,0,1); }";
    }
    PipelineCache cache(dev);
    ShaderManager mgr(dev, cache);
    Shader* shader = mgr.load(tmp.string(), rhi::ShaderStage::Fragment);
    NF_CHECK(shader != nullptr);
    uint64_t v1 = shader->version();
    rhi::ShaderModule* mod1 = shader->module();
    NF_CHECK(mod1 != nullptr);

    // Now corrupt the file to cause compile failure
    {
        std::ofstream out(tmp);
        out << "#version 450\n this is not valid glsl !!!";
    }
    bool ok = mgr.reload(tmp.string());
    NF_CHECK(!ok); // reload should fail
    // Version and module must be unchanged
    NF_CHECK_EQ(shader->version(), v1);
    NF_CHECK(shader->module() == mod1);

    std::filesystem::remove(tmp);
}

NF_TEST(shader_reload_rebuilds_pipeline) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    rhi::TextureDesc td{}; td.width=8; td.height=8; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt;
    auto tex=dev.create_texture(td);
    rhi::ColorAttachment ca{}; ca.format=td.format;
    const std::array<rhi::ColorAttachment,1> atts{ca};
    rhi::RenderPassDesc rpd{}; rpd.color_attachments=std::span<const rhi::ColorAttachment>(atts); rpd.present_source=false;
    auto rp=dev.create_render_pass(rpd);

    // Create a temp shader file
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "nf_test_reload.frag";
    {
        std::ofstream out(tmp);
        out << "#version 450\nlayout(location=0) out vec4 c; void main(){ c = vec4(1,0,0,1); }";
    }
    PipelineCache cache(dev);
    ShaderManager mgr(dev, cache);
    Shader* shader = mgr.load(tmp.string(), rhi::ShaderStage::Fragment);
    NF_CHECK(shader != nullptr);

    auto vc = load_spirv(std::filesystem::path(NF_RHI_TEST_QUAD_SHADER_DIR) / "textured_quad_vert.spv");
    auto vs = dev.create_shader_module({vc, rhi::ShaderStage::Vertex});

    const std::array<rhi::DescriptorBinding,1> qbinds{{{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc qld{}; qld.bindings = std::span<const rhi::DescriptorBinding>(qbinds);
    auto set_layout = dev.create_descriptor_set_layout(qld); NF_CHECK(set_layout);

    const std::array<rhi::VertexAttrib,2> qattribs{{
        {0, 0, rhi::Format::R32G32_SFloat},
        {1, 8, rhi::Format::R32G32_SFloat}
    }};
    rhi::VertexLayout qvl{}; qvl.binding=0; qvl.stride=16; qvl.attributes=std::span<const rhi::VertexAttrib>(qattribs);

    rhi::PipelineDesc pd{};
    pd.vs=vs.get();
    pd.fs=shader->module();
    pd.render_pass=rp.get();
    pd.descriptor_set_layout=set_layout.get();
    pd.vertex_layout=qvl;
    rhi::Pipeline* p1 = cache.get_or_create(pd);
    NF_CHECK(p1 != nullptr);
    NF_CHECK(cache.size()==1);

    // Modify the shader to a different color (still valid) and reload
    {
        std::ofstream out(tmp);
        out << "#version 450\nlayout(location=0) out vec4 c; void main(){ c = vec4(0,1,0,1); }";
    }
    bool ok = mgr.reload(tmp.string());
    NF_CHECK(ok);
    NF_CHECK(shader->version() == 2);
    // Old pipeline should have been invalidated (it used the old shader module)
    // The cache should now be empty or not contain the old pipeline
    // Since we invalidated, size should be 0 before we create the new pipeline
    // Actually invalidate_shader removes the old pipeline, so size should be 0
    // Let's check: after reload, cache should have 0 or 1? The old pipeline was invalidated, but we haven't created a new one yet
    NF_CHECK(cache.size()==0);
    // Now create a pipeline with the new shader module — it should be a new pipeline
    pd.fs = shader->module();
    rhi::Pipeline* p2 = cache.get_or_create(pd);
    NF_CHECK(p2 != nullptr);
    NF_CHECK(cache.size()==1);
    // (p2 != p1 is not asserted: p1 was freed and its address may be reused.)

    std::filesystem::remove(tmp);
}

NF_TEST(shader_async_compile_via_job_system) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    // Ensure JobSystem is running
    if (!nf::JobSystem::instance().is_initialized()) {
        nf::JobSystem::instance().init(2);
    }

    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "nf_test_async.frag";
    {
        std::ofstream out(tmp);
        out << "#version 450\nlayout(location=0) out vec4 c; void main(){ c = vec4(0,0,1,1); }";
    }
    PipelineCache cache(dev);
    ShaderManager mgr(dev, cache);
    Shader* shader = mgr.load(tmp.string(), rhi::ShaderStage::Fragment);
    NF_CHECK(shader != nullptr);
    uint64_t v1 = shader->version();

    // Modify file and poll (which enqueues async job)
    {
        std::ofstream out(tmp);
        out << "#version 450\nlayout(location=0) out vec4 c; void main(){ c = vec4(1,1,0,1); }";
    }
    // Ensure file time changes (some filesystems have 1s granularity)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mgr.poll();
    // Give the job a moment to compile
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Commit on main thread
    size_t updated = mgr.update();
    NF_CHECK(updated==1);
    NF_CHECK(shader->version() == v1+1);

    std::filesystem::remove(tmp);
}
