#pragma once

// NF/RHI/Vulkan/VulkanObjects.hpp — Concrete Vulkan implementations of the
// abstract RHI interfaces declared in NF/RHI/RHI.hpp.
//
// These headers are internal to the RHI backend (they live under src/, not
// include/) and must never be visible to the engine or editor.

#include "VulkanLoader.hpp"

#include <NF/RHI/RHI.hpp>
#include <NF/RHI/Vulkan/Device_Vk.hpp>

#include <vector>

namespace nf::rhi {

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------
class VulkanBuffer final : public Buffer {
public:
    VulkanBuffer(VulkanDevice* device, const BufferDesc& desc);
    ~VulkanBuffer() override;

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    bool valid() const { return m_buffer != VK_NULL_HANDLE; }

    void* map() override;
    void unmap() override;
    void update(const void* data, usize offset, usize size) override;
    usize size() const override { return m_size; }

    VkBuffer handle() const { return m_buffer; }
    MemoryUsage memory_usage() const { return m_memory_usage; }

private:
    void release();

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    usize m_size = 0;
    MemoryUsage m_memory_usage = MemoryUsage::GPUOnly;
    void* m_mapped = nullptr;
};

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

/// Process-wide monotonic texture serial (never reused while the process
/// lives, unlike heap addresses). Defined once in Texture_Vk.cpp.
u64 next_texture_serial();

class VulkanTexture final : public Texture {
public:
    VulkanTexture(VulkanDevice* device, const TextureDesc& desc);

    /// Wraps an existing VkImage (used for swapchain images, which the
    /// swapchain owns and destroys).
    VulkanTexture(VulkanDevice* device, VkImage image, VkFormat format,
                  u32 width, u32 height, bool owned);

    ~VulkanTexture() override;

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    bool valid() const { return m_image != VK_NULL_HANDLE; }

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    Format format() const override { return m_format; }

    VkImage image() const { return m_image; }
    VkImageView view() const { return m_view; }
    VkFormat vk_format() const { return m_vk_format; }
    u32 mip_levels() const { return m_mip_levels; }
    u64 creation_serial() const override { return m_serial; }

    /// Current layout — required to emit correct image memory barriers.
    ///
    /// The layout is bookkeeping about a GPU-side resource, not part of the
    /// object's value, so it is mutated through const references: recording a
    /// readback must be able to update it on a texture the command buffer only
    /// holds by const reference.
    VkImageLayout layout() const { return m_layout; }
    void set_layout(VkImageLayout layout) const { m_layout = layout; }

private:
    void release();
    bool create_view(bool is_depth);

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkFormat m_vk_format = VK_FORMAT_UNDEFINED;
    Format m_format = Format::Unknown;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_mip_levels = 1;
    mutable VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_owned = true;
    const u64 m_serial = next_texture_serial();
};

// ---------------------------------------------------------------------------
// TextureView
// ---------------------------------------------------------------------------
//
// A view into a VulkanTexture. Supports 2D / Array / Cube / 3D, arbitrary mip
// and layer ranges, and explicit aspect selection so depth/stencil textures
// can be viewed correctly.
class VulkanTextureView final : public TextureView {
public:
    /// Legacy path — creates a 2D color view of mip 0 / layer 0.
    VulkanTextureView(VulkanDevice* device, const Texture& texture);
    /// Generic path — fully described view.
    VulkanTextureView(VulkanDevice* device, const TextureViewDesc& desc);
    ~VulkanTextureView() override;

    VulkanTextureView(const VulkanTextureView&) = delete;
    VulkanTextureView& operator=(const VulkanTextureView&) = delete;

    bool valid() const { return m_view != VK_NULL_HANDLE; }

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    Format format() const override { return m_format; }

    VkImageView view() const { return m_view; }
    VkFormat vk_format() const { return m_vk_format; }
    ViewDimension dimension() const { return m_dimension; }
    ImageAspect aspect() const { return m_aspect; }

private:
    bool create_from_desc(const TextureViewDesc& desc);

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkImageView m_view = VK_NULL_HANDLE;
    VkFormat m_vk_format = VK_FORMAT_UNDEFINED;
    Format m_format = Format::Unknown;
    u32 m_width = 0;
    u32 m_height = 0;
    ViewDimension m_dimension = ViewDimension::View2D;
    ImageAspect m_aspect = ImageAspect::Color;
};

// ---------------------------------------------------------------------------
// Shader module
// ---------------------------------------------------------------------------
class VulkanShaderModule final : public ShaderModule {
public:
    VulkanShaderModule(VulkanDevice* device, const ShaderModuleDesc& desc);
    ~VulkanShaderModule() override;

    VulkanShaderModule(const VulkanShaderModule&) = delete;
    VulkanShaderModule& operator=(const VulkanShaderModule&) = delete;

    bool valid() const { return m_module != VK_NULL_HANDLE; }

    ShaderStage stage() const override { return m_stage; }
    VkShaderModule handle() const { return m_module; }
    const char* entry_point() const { return "main"; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkShaderModule m_module = VK_NULL_HANDLE;
    ShaderStage m_stage = ShaderStage::Vertex;
};

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
class VulkanPipeline final : public Pipeline {
public:
    VulkanPipeline(VulkanDevice* device, const PipelineDesc& desc);
    ~VulkanPipeline() override;

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    bool valid() const { return m_pipeline != VK_NULL_HANDLE; }

    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }
    VkPipelineBindPoint bind_point() const { return VK_PIPELINE_BIND_POINT_GRAPHICS; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// Render pass
// ---------------------------------------------------------------------------
class VulkanRenderPass final : public RenderPass {
public:
    VulkanRenderPass(VulkanDevice* device, const RenderPassDesc& desc);
    ~VulkanRenderPass() override;

    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;

    bool valid() const { return m_render_pass != VK_NULL_HANDLE; }

    VkRenderPass handle() const { return m_render_pass; }

    RenderPassDesc desc() const override {
        // Re-point the span at our own storage: the caller's array may be gone.
        RenderPassDesc d{};
        d.color_attachments = std::span<const ColorAttachment>(m_color_attachments);
        d.depth_format = m_depth_format;
        d.has_depth = m_has_depth;
        d.present_source = m_present_source;
        d.color_load = m_color_load;
        return d;
    }
    u32 color_attachment_count() const override {
        return static_cast<u32>(m_color_attachments.size());
    }
    bool has_depth() const override { return m_has_depth; }

    /// Layout the color attachments are left in when the pass ends. Command
    /// recording needs it to keep the images' tracked layouts honest.
    VkImageLayout final_color_layout() const {
        return m_present_source ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkRenderPass m_render_pass = VK_NULL_HANDLE;
    Format m_depth_format = Format::Unknown;
    bool m_has_depth = false;
    bool m_present_source = true;
    RenderPassDesc::ColorLoad m_color_load = RenderPassDesc::ColorLoad::Clear;
    std::vector<ColorAttachment> m_color_attachments; // owning copy
};

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------
class VulkanFramebuffer final : public Framebuffer {
public:
    VulkanFramebuffer(VulkanDevice* device, const VulkanRenderPass& pass,
                      std::span<Texture* const> color_attachments,
                      Texture* depth_attachment);
    ~VulkanFramebuffer() override;

    VulkanFramebuffer(const VulkanFramebuffer&) = delete;
    VulkanFramebuffer& operator=(const VulkanFramebuffer&) = delete;

    bool valid() const { return m_framebuffer != VK_NULL_HANDLE; }

    VkFramebuffer handle() const { return m_framebuffer; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    /// The attachments this framebuffer writes to. Command recording needs
    /// them to update each image's tracked layout, because a render pass
    /// performs its layout transitions implicitly and the tracker would
    /// otherwise still believe the image is in its pre-pass layout.
    std::span<Texture* const> color_attachments() const {
        return std::span<Texture* const>(m_color_attachments);
    }
    Texture* depth_attachment() const { return m_depth_attachment; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    u32 m_width = 0;
    u32 m_height = 0;
    std::vector<Texture*> m_color_attachments; // non-owning
    Texture* m_depth_attachment = nullptr;     // non-owning
};

// ---------------------------------------------------------------------------
// Command buffer
// ---------------------------------------------------------------------------
class VulkanCommandBuffer final : public CommandBuffer {
public:
    /// Each command buffer owns a private VkCommandPool. Sharing one pool
    /// across command buffers lets a freed-then-reallocated raw handle stay
    /// tainted (per the validation layer's lifetime tracking) by objects a
    /// previous command buffer referenced — private pools make that class of
    /// cross-test/cross-frame poisoning structurally impossible.
    explicit VulkanCommandBuffer(VulkanDevice* device);
    ~VulkanCommandBuffer() override;

    VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
    VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

    bool valid() const { return m_cmd != VK_NULL_HANDLE; }

    void reset() override;
    void begin() override;
    void end() override;

    void begin_render_pass(const RenderPass& pass, const Framebuffer& fb,
                           std::span<const ClearValue> clear_values,
                           float clear_depth = 1.0f, u8 clear_stencil = 0) override;
    void end_render_pass() override;

    void bind_pipeline(const Pipeline& pipeline) override;
    void bind_vertex_buffers(std::span<const Buffer* const> buffers) override;
    void bind_index_buffer(const Buffer& buffer, usize offset = 0) override;

    void bind_descriptor_sets(const DescriptorSetLayout& layout,
                             std::span<const DescriptorSet* const> sets,
                             u32 first_set = 0) override;

    void push_constants(ShaderStage stages, u32 offset, u32 size,
                        const void* data) override;

    void set_viewport(u32 x, u32 y, u32 width, u32 height,
                      float min_depth = 0.0f, float max_depth = 1.0f) override;
    void set_scissor(u32 x, u32 y, u32 width, u32 height) override;

    void draw(u32 vertex_count, u32 instance_count = 1,
              u32 first_vertex = 0, u32 first_instance = 0) override;
    void draw_indexed(u32 index_count, u32 instance_count = 1,
                      u32 first_index = 0, i32 vertex_offset = 0,
                      u32 first_instance = 0) override;

    void copy_buffer(const Buffer& src, Buffer& dst,
                     usize src_offset, usize dst_offset, usize size) override;
    void copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                 usize buffer_offset,
                                 u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h) override;
    void transition_texture_for_sampling(Texture& texture) override;
    void barrier_texture(Texture& texture, ImageUsage before, ImageUsage after) override;
    void copy_texture_to_buffer(const Texture& src, Buffer& dst,
                                u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h,
                                usize buffer_offset = 0) override;
    bool generate_mipmaps(Texture& texture) override;

    VkCommandBuffer handle() const { return m_cmd; }

    /// Raw escape hatch for backend-internal work (uploads, layout
    /// transitions) that the abstract interface does not model yet.
    VkCommandBuffer raw() const { return m_cmd; }

private:
    void transition_image_layout(VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout,
                                 VkImageAspectFlags aspect);

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    bool m_recording = false;

    /// Pipeline layout of the most recently bound pipeline. vkCmdPushConstants
    /// needs a VkPipelineLayout, and the RHI's push_constants() deliberately
    /// does not take one — callers push to "whatever pipeline is bound", which
    /// is how every backend that is not Vulkan models it too.
    VkPipelineLayout m_current_layout = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// Semaphore / Fence
// ---------------------------------------------------------------------------
class VulkanSemaphore final : public Semaphore {
public:
    explicit VulkanSemaphore(VulkanDevice* device);
    ~VulkanSemaphore() override;

    VulkanSemaphore(const VulkanSemaphore&) = delete;
    VulkanSemaphore& operator=(const VulkanSemaphore&) = delete;

    bool valid() const { return m_semaphore != VK_NULL_HANDLE; }
    VkSemaphore handle() const { return m_semaphore; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
};

class VulkanFence final : public Fence {
public:
    explicit VulkanFence(VulkanDevice* device, bool signaled);
    ~VulkanFence() override;

    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    bool valid() const { return m_fence != VK_NULL_HANDLE; }

    bool wait(u64 timeout_ns = u64_max) override;
    void reset() override;
    bool is_signaled() override;

    VkFence handle() const { return m_fence; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkFence m_fence = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------
class VulkanSampler final : public Sampler {
public:
    VulkanSampler(VulkanDevice* device, const SamplerDesc& desc);
    ~VulkanSampler() override;

    VulkanSampler(const VulkanSampler&) = delete;
    VulkanSampler& operator=(const VulkanSampler&) = delete;

    bool valid() const { return m_sampler != VK_NULL_HANDLE; }
    VkSampler handle() const { return m_sampler; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// DescriptorSetLayout
// ---------------------------------------------------------------------------
class VulkanDescriptorSetLayout final : public DescriptorSetLayout {
public:
    VulkanDescriptorSetLayout(VulkanDevice* device, const DescriptorSetLayoutDesc& desc);
    ~VulkanDescriptorSetLayout() override;

    VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout&) = delete;
    VulkanDescriptorSetLayout& operator=(const VulkanDescriptorSetLayout&) = delete;

    bool valid() const { return m_layout != VK_NULL_HANDLE; }
    VkDescriptorSetLayout handle() const { return m_layout; }

    /// The bindings this layout declares. Descriptor sets need them to size
    /// their pool: a VkDescriptorPoolSize entry must exist for every
    /// descriptor type the layout actually uses, with enough descriptors for
    /// every binding of that type.
    std::span<const DescriptorBinding> bindings() const {
        return std::span<const DescriptorBinding>(m_bindings);
    }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> m_bindings; // owning copy
};

// ---------------------------------------------------------------------------
// DescriptorSet
// ---------------------------------------------------------------------------
class VulkanDescriptorSet final : public DescriptorSet {
public:
    VulkanDescriptorSet(VulkanDevice* device, const VulkanDescriptorSetLayout& layout);
    ~VulkanDescriptorSet() override;

    VulkanDescriptorSet(const VulkanDescriptorSet&) = delete;
    VulkanDescriptorSet& operator=(const VulkanDescriptorSet&) = delete;

    bool valid() const { return m_set != VK_NULL_HANDLE; }
    VkDescriptorSet handle() const { return m_set; }

private:
    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;  // Owned by this set (simple 1:1 model)
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// DescriptorAllocator (growable, reset per frame)
// ---------------------------------------------------------------------------
class VulkanDescriptorSetPooled final : public DescriptorSet {
public:
    VulkanDescriptorSetPooled(VkDescriptorSet set) : m_set(set) {}
    bool valid() const { return m_set != VK_NULL_HANDLE; }
    VkDescriptorSet handle() const { return m_set; }
private:
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

class VulkanDescriptorAllocator final : public DescriptorAllocator {
public:
    VulkanDescriptorAllocator(VulkanDevice* device, u32 sets_per_pool);
    ~VulkanDescriptorAllocator() override;

    VulkanDescriptorAllocator(const VulkanDescriptorAllocator&) = delete;
    VulkanDescriptorAllocator& operator=(const VulkanDescriptorAllocator&) = delete;

    std::unique_ptr<DescriptorSet> allocate(const DescriptorSetLayout& layout) override;
    void reset() override;
    u32 allocated_count() const override { return m_allocated; }
    u32 pool_count() const override { return static_cast<u32>(m_pools.size()); }

private:
    struct Pool {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        u32 remaining = 0;
    };
    bool create_pool_for_layout(const VulkanDescriptorSetLayout& layout, Pool& out);

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    u32 m_sets_per_pool = 32;
    std::vector<Pool> m_pools;
    u32 m_allocated = 0;
};

// ---------------------------------------------------------------------------
// UploadContext (async staging, submit returns fence)
// ---------------------------------------------------------------------------
class VulkanUploadContext final : public UploadContext {
public:
    explicit VulkanUploadContext(VulkanDevice* device);
    ~VulkanUploadContext() override;

    VulkanUploadContext(const VulkanUploadContext&) = delete;
    VulkanUploadContext& operator=(const VulkanUploadContext&) = delete;

    void copy_buffer(const Buffer& src, Buffer& dst,
                     usize src_offset, usize dst_offset, usize size) override;
    void copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                usize buffer_offset,
                                u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h) override;
    std::unique_ptr<Fence> submit() override;

    bool valid() const { return m_cmd != VK_NULL_HANDLE; }

private:
    void ensure_began();
    void transition_for_copy(VulkanTexture& tex, VkImageLayout target);

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    bool m_began = false;
    bool m_submitted = false;
};

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
class VulkanSwapchain final : public Swapchain {
public:
    VulkanSwapchain(VulkanDevice* device, const SwapchainDesc& desc);
    ~VulkanSwapchain() override;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    bool valid() const { return m_swapchain != VK_NULL_HANDLE; }

    u32 acquire_next_image(const Semaphore& signal_semaphore) override;
    void present(u32 image_index, std::span<const Semaphore* const> wait_semaphores) override;

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    Format format() const override { return m_format; }
    u32 image_count() const override { return static_cast<u32>(m_textures.size()); }
    Texture* get_texture(u32 index) override;

    VkSwapchainKHR handle() const { return m_swapchain; }

private:
    void release_textures();

    VulkanDevice* m_device = nullptr;
    VkAliveGuard m_alive;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    SwapchainDesc m_desc{};
    u32 m_width = 0;
    u32 m_height = 0;
    Format m_format = Format::Unknown;
    std::vector<std::unique_ptr<VulkanTexture>> m_textures;
};

} // namespace nf::rhi
