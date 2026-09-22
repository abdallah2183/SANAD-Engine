#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Assert.hpp>

#include <queue>
#include <unordered_map>

namespace nf::rendering {

RenderGraph::RenderGraph(rhi::IGraphicsDevice& device) : m_device(device) {}

RenderGraph::~RenderGraph() = default;

RGTextureHandle RenderGraph::create_texture(const std::string& name, const rhi::TextureDesc& desc) {
    Resource res;
    res.name = name;
    res.desc = desc;
    res.imported = false;
    m_resources.push_back(std::move(res));
    m_compiled = false;
    return static_cast<RGTextureHandle>(m_resources.size() - 1);
}

RGTextureHandle RenderGraph::import_texture(const std::string& name, rhi::Texture* external) {
    NF_ASSERT(external, "import_texture requires a valid external texture");
    Resource res;
    res.name = name;
    res.desc = {};
    res.desc.width = external->width();
    res.desc.height = external->height();
    res.desc.format = external->format();
    res.external = external;
    res.imported = true;
    m_resources.push_back(std::move(res));
    m_compiled = false;
    return static_cast<RGTextureHandle>(m_resources.size() - 1);
}

u32 RenderGraph::add_pass(const RGPassDesc& desc) {
    Pass p;
    p.name = desc.name;
    p.reads = desc.reads;
    p.writes = desc.writes;
    p.color_attachments = desc.color_attachments;
    p.depth_attachment = desc.depth_attachment;
    p.shader_writes = desc.shader_writes;
    p.execute = desc.execute;
    // Treat color attachments as writes if not already in writes
    for (auto h : p.color_attachments) {
        bool found = false;
        for (auto w : p.writes) if (w == h) { found = true; break; }
        if (!found) p.writes.push_back(h);
    }
    // The depth attachment and storage writes participate in dependency
    // tracking exactly like any other write.
    if (p.depth_attachment != kInvalidRGHandle) {
        bool found = false;
        for (auto w : p.writes) if (w == p.depth_attachment) { found = true; break; }
        if (!found) p.writes.push_back(p.depth_attachment);
    }
    for (auto h : p.shader_writes) {
        bool found = false;
        for (auto w : p.writes) if (w == h) { found = true; break; }
        if (!found) p.writes.push_back(h);
    }
    m_passes.push_back(std::move(p));
    m_compiled = false;
    return static_cast<u32>(m_passes.size() - 1);
}

rhi::Texture* RenderGraph::get_texture(RGTextureHandle handle) {
    if (handle == kInvalidRGHandle || handle >= m_resources.size()) return nullptr;
    return m_resources[handle].texture();
}

const rhi::Texture* RenderGraph::get_texture(RGTextureHandle handle) const {
    if (handle == kInvalidRGHandle || handle >= m_resources.size()) return nullptr;
    return m_resources[handle].texture();
}

bool RenderGraph::build_execution_order() {
    const size_t n = m_passes.size();
    if (n == 0) {
        m_execution_order.clear();
        return true;
    }

    // Build adjacency and indegree
    std::vector<std::vector<u32>> adj(n);
    std::vector<int> indegree(n, 0);

    // Dependency model (registration order = program order, with one
    // dataflow exception):
    // 1. WAW: writers of one resource execute in registration order
    //    (w[i] -> w[i+1]). Two writers of one target — Lighting then
    //    Transparency over one HDR image — must not order by luck of the
    //    queue's tie-break in Kahn's algorithm below.
    // 2. RAW: a reader consumes the version its nearest PRIOR writer produced
    //    (last writer with index < reader). A later writer has not run yet
    //    when the reader executes, so resolving against the LAST writer
    //    overall would point the edge backwards and report a cycle in a
    //    graph that has no dataflow loop (GBuffer writes depth, Lighting
    //    reads it, Transparency writes it again).
    // 3. WAR: a reader must run before the next writer OVERWRITES the version
    //    it just read (reader -> first writer with index > reader). Without
    //    it the later writer may run first and the reader samples garbage.
    // 4. Out-of-order single producer: a reader registered BEFORE any writer
    //    of that resource takes its data from the first writer (writer ->
    //    reader, backwards in registration order). This is what lets passes
    //    be added dependency-first and still execute writer-first; it only
    //    applies when no prior writer exists, so it can never contradict a
    //    RAW edge on the same resource.
    // Rules 1-3 only ever point forward in registration order; rule 4 points
    // backwards only where no forward RAW edge exists for that reader, so the
    // combination is acyclic for any acyclic dataflow.
    std::unordered_map<RGTextureHandle, std::vector<u32>> writers_of;
    writers_of.reserve(n * 2);
    for (u32 pass_idx = 0; pass_idx < n; ++pass_idx) {
        for (RGTextureHandle w : m_passes[pass_idx].writes) {
            writers_of[w].push_back(pass_idx);
        }
    }

    auto add_edge = [&](u32 from, u32 to) {
        if (from == to) return;
        auto& edges = adj[from];
        if (std::find(edges.begin(), edges.end(), to) == edges.end()) {
            edges.push_back(to);
            ++indegree[to];
        }
    };

    // WAW chains.
    for (const auto& [handle, writers] : writers_of) {
        (void)handle;
        for (size_t i = 0; i + 1 < writers.size(); ++i) {
            add_edge(writers[i], writers[i + 1]);
        }
    }

    // RAW + WAR (+ out-of-order producer) per reader.
    for (u32 pass_idx = 0; pass_idx < n; ++pass_idx) {
        const Pass& pass = m_passes[pass_idx];
        for (RGTextureHandle r : pass.reads) {
            auto it = writers_of.find(r);
            if (it == writers_of.end()) continue;
            const std::vector<u32>& writers = it->second;
            // Nearest writer strictly before this pass (its version supplier).
            u32 prior = kInvalidRGHandle;
            // Nearest writer strictly after this pass.
            u32 next = kInvalidRGHandle;
            for (u32 w : writers) {
                if (w == pass_idx) continue; // read-modify-write: self is not its own supplier
                if (w < pass_idx) prior = w; // writers are in registration order: last one wins
                else if (next == kInvalidRGHandle) next = w; // first one wins
            }
            if (prior != kInvalidRGHandle) {
                add_edge(prior, pass_idx); // RAW: correct version, not the last writer overall
                if (next != kInvalidRGHandle) {
                    add_edge(pass_idx, next); // WAR: read before the overwrite
                }
            } else if (next != kInvalidRGHandle) {
                add_edge(next, pass_idx); // out-of-order producer: writer runs first
                // WAR to the writers after the producer (if any): the reader
                // consumes the producer's version, so later overwrites wait.
                bool seen_producer = false;
                for (u32 w : writers) {
                    if (w == pass_idx) continue;
                    if (!seen_producer) {
                        if (w == next) seen_producer = true;
                        continue;
                    }
                    add_edge(pass_idx, w);
                }
            }
        }
    }

    // Kahn's topological sort - preserve original order where possible for stability
    std::queue<u32> q;
    for (u32 i = 0; i < n; ++i) if (indegree[i]==0) q.push(i);

    m_execution_order.clear();
    m_execution_order.reserve(n);
    while (!q.empty()) {
        u32 u = q.front(); q.pop();
        m_execution_order.push_back(u);
        for (u32 v : adj[u]) {
            if (--indegree[v]==0) q.push(v);
        }
    }

    if (m_execution_order.size() != n) {
        NF_LOG_ERROR(LogCategory::Core, "RenderGraph: cycle detected ({} passes, ordered {})", n, m_execution_order.size());
        return false;
    }

    NF_LOG_TRACE(LogCategory::Core, "RenderGraph compiled: {} passes in order", n);
    for (size_t i=0;i<m_execution_order.size();++i) {
        NF_LOG_TRACE(LogCategory::Core, "  [{}] {}", i, m_passes[m_execution_order[i]].name);
    }
    return true;
}

bool RenderGraph::compile() {
    if (m_compiled) return true;

    // Create owned textures
    for (auto& res : m_resources) {
        if (res.imported) continue;
        if (res.owned_texture) continue; // already created
        auto tex = m_device.create_texture(res.desc);
        if (!tex) {
            NF_LOG_ERROR(LogCategory::Core, "RenderGraph: failed to create texture '{}'", res.name);
            return false;
        }
        res.owned_texture = std::move(tex);
        NF_LOG_TRACE(LogCategory::Core, "RenderGraph: created texture '{}' {}x{}", res.name, res.desc.width, res.desc.height);
    }

    if (!build_execution_order()) {
        return false;
    }

    m_compiled = true;
    return true;
}

void RenderGraph::execute(rhi::CommandBuffer& cmd) {
    NF_ASSERT(m_compiled, "RenderGraph::execute called before compile()");
    if (!m_compiled) {
        if (!compile()) return;
    }

    for (u32 pass_idx : m_execution_order) {
        const Pass& pass = m_passes[pass_idx];
        NF_LOG_TRACE(LogCategory::Core, "RenderGraph: executing pass '{}'", pass.name);

        // Generic barrier inference: for each read, transition from current_state to ShaderRead
        for (RGTextureHandle r : pass.reads) {
            if (r >= m_resources.size()) continue;
            Resource& res = m_resources[r];
            rhi::Texture* tex = res.texture();
            if (!tex) continue;
            if (res.current_state != RGResourceState::ShaderRead) {
                auto trans = get_transition(res.current_state, RGResourceState::ShaderRead);
                cmd.barrier_texture(*tex, trans.before, trans.after);
                res.current_state = RGResourceState::ShaderRead;
            }
        }
        // For each write, transition to the state implied by the role the pass
        // declared. The graph never inspects texture formats or backend types:
        //   depth_attachment → DepthAttachment
        //   color_attachments → ColorAttachment
        //   shader_writes    → ShaderWrite (storage)
        //   remaining writes → TransferDst (pure transfer passes)
        for (RGTextureHandle w : pass.writes) {
            if (w >= m_resources.size()) continue;
            Resource& res = m_resources[w];
            rhi::Texture* tex = res.texture();
            if (!tex) continue;
            RGResourceState desired = RGResourceState::TransferDst;
            if (pass.depth_attachment == w) {
                desired = RGResourceState::DepthAttachment;
            } else {
                for (auto ca : pass.color_attachments) if (ca == w) { desired = RGResourceState::ColorAttachment; break; }
                if (desired == RGResourceState::TransferDst) {
                    for (auto sw : pass.shader_writes) if (sw == w) { desired = RGResourceState::ShaderWrite; break; }
                }
            }
            if (res.current_state != desired) {
                auto trans = get_transition(res.current_state, desired);
                // Only issue barrier if before != Undefined or after != Undefined and they differ
                if (trans.before != rhi::ImageUsage::None || trans.after != rhi::ImageUsage::None) {
                    cmd.barrier_texture(*tex, trans.before, trans.after);
                }
                res.current_state = desired;
            }
        }

        // Execute the pass
        if (pass.execute) {
            pass.execute(cmd);
        }

        // After the pass, update the state of written resources to reflect what the pass did
        // For color attachments, after the pass they remain ColorAttachment until next read
        // For transfer dst, after copy they remain TransferDst (the copy itself leaves it there)
        // The next read will transition to ShaderRead as above
    }
}

void RenderGraph::reset(bool keep_resources) {
    m_passes.clear();
    m_execution_order.clear();
    m_compiled = false;
    if (!keep_resources) {
        m_resources.clear();
    }
    // If keep_resources, we keep owned textures alive for reuse; passes are cleared.
}

} // namespace nf::rendering
