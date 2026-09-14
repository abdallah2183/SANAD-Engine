#pragma once

// NF/RHI/Vulkan/Device_Vk.hpp — Vulkan graphics device implementation

#include <NF/RHI/RHI.hpp>

#ifndef VK_NO_PROTOTYPES
    #define VK_NO_PROTOTYPES
#endif

#ifdef _WIN32
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
#endif

#include <vulkan/vulkan.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace nf::rhi {

class VulkanBuffer;
class VulkanTexture;
class VulkanTextureView;
class VulkanShaderModule;
class VulkanPipeline;
class VulkanRenderPass;
class VulkanFramebuffer;
class VulkanCommandBuffer;
class VulkanSwapchain;
class VulkanSampler;
class VulkanDescriptorSetLayout;
class VulkanDescriptorSet;
class VulkanDescriptorAllocator;
class VulkanUploadContext;

// Holds all Vk handles for a logical device + queues
struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    u32 graphics_family = 0;
    u32 present_family = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;

    VkCommandPool command_pool = VK_NULL_HANDLE;

    // Memory + device properties (queried once at device selection)
    VkPhysicalDeviceProperties device_properties{};
    VkPhysicalDeviceMemoryProperties memory_props{};

    // Enabled features
    bool enable_validation = false;

    // Dynamic dispatch
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
};

class VulkanDevice : public IGraphicsDevice, public nf::NonMovable {
public:
    VulkanDevice();
    ~VulkanDevice() override;

    bool init(void* native_window_handle) override;
    bool init(const DeviceDesc& desc) override;
    void shutdown() override;

    std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) override;
    std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) override;
    std::unique_ptr<Texture> create_texture(const TextureDesc& desc) override;
    std::unique_ptr<TextureView> create_texture_view(const Texture& texture) override;
    std::unique_ptr<TextureView> create_texture_view(const TextureViewDesc& desc) override;
    std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& desc) override;
    std::unique_ptr<Pipeline> create_pipeline(const PipelineDesc& desc) override;
    std::unique_ptr<RenderPass> create_render_pass(const RenderPassDesc& desc) override;
    std::unique_ptr<Framebuffer> create_framebuffer(const RenderPass& pass,
                                                      std::span<Texture* const> color_attachments,
                                                      Texture* depth_attachment) override;
    std::unique_ptr<Sampler> create_sampler(const SamplerDesc& desc) override;
    std::unique_ptr<DescriptorSetLayout> create_descriptor_set_layout(
        const DescriptorSetLayoutDesc& desc) override;
    std::unique_ptr<DescriptorSet> create_descriptor_set(
        const DescriptorSetLayout& layout) override;
    std::unique_ptr<DescriptorAllocator> create_descriptor_allocator(u32 sets_per_pool = 32) override;
    std::unique_ptr<UploadContext> create_upload_context() override;
    void update_descriptor_set(const DescriptorSet& set,
                              std::span<const DescriptorWrite> writes) override;
    std::unique_ptr<CommandBuffer> create_command_buffer() override;
    std::unique_ptr<Semaphore> create_semaphore() override;
    std::unique_ptr<Fence> create_fence(bool signaled) override;

    void submit(const CommandBuffer& cmd, const SubmitInfo& info) override;
    void wait_idle() override;

    const char* backend_name() const override { return "Vulkan"; }

    bool validation_enabled() const override { return m_ctx.enable_validation; }
    u32 alive_objects() const override { return m_alive_objects.load(std::memory_order_relaxed); }

    /// Bookkeeping for the alive-object counter. Every backend object calls
    /// track_alive(+1) from its constructor and (-1) from its VkAliveGuard
    /// destructor, so the count reflects exactly the objects the caller owns.
    void track_alive(int delta) { m_alive_objects.fetch_add(delta, std::memory_order_relaxed); }

    VulkanContext& context() { return m_ctx; }
    const VulkanContext& context() const { return m_ctx; }

    u32 find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) const;

    VkQueue graphics_queue() const { return m_ctx.graphics_queue; }
    VkQueue present_queue() const { return m_ctx.present_queue; }

    /// Records, submits and waits on a short-lived command buffer.
    /// Intended for uploads and layout transitions that are not tied to a
    /// frame — it is synchronous, so never use it inside the render loop.
    bool execute_single_time_commands(const std::function<void(VkCommandBuffer)>& record);

private:
    bool create_instance();
    bool setup_debug_messenger();
    bool create_surface(void* hwnd);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_command_pool();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* user_data);

    VulkanContext m_ctx;
    bool m_requested_validation = false;
    std::atomic<u32> m_alive_objects{0};
};

/// RAII helper owned by every backend object: registers the object with its
/// device on attach() and unregisters on destruction. Default-constructible
/// so classes can embed it as a member and attach it in their constructor
/// body once m_device is known. VkDescriptorSetPooled embeds none: it is a
/// lightweight view over an allocator-owned VkDescriptorSet, not a resource.
struct VkAliveGuard {
    VkAliveGuard() = default;
    explicit VkAliveGuard(VulkanDevice* device) { attach(device); }
    ~VkAliveGuard() {
        if (m_device) m_device->track_alive(-1);
    }
    VkAliveGuard(const VkAliveGuard&) = delete;
    VkAliveGuard& operator=(const VkAliveGuard&) = delete;

    void attach(VulkanDevice* device) {
        m_device = device;
        if (m_device) m_device->track_alive(1);
    }

    VulkanDevice* m_device = nullptr;
};

// Convert helpers — shared by every translation unit in the Vulkan backend.
VkFormat to_vk_format(Format fmt);
Format from_vk_format(VkFormat fmt);
VkShaderStageFlagBits to_vk_shader_stage(ShaderStage stage);
VkPrimitiveTopology to_vk_topology(PrimitiveTopology topo);
VkCullModeFlags to_vk_cull_mode(CullMode mode);
VkFrontFace to_vk_front_face(FrontFace ff);
VkCompareOp to_vk_compare_op(CompareOp op);
VkImageUsageFlags to_vk_image_usage(ImageUsage usage);
VkBufferUsageFlags to_vk_buffer_usage(BufferUsage usage);
VkPresentModeKHR to_vk_present_mode(PresentMode mode);
VkBlendOp to_vk_blend_op(BlendOp op);
VkBlendFactor to_vk_blend_factor(BlendFactor f);
VkFilter to_vk_filter(Filter f);
VkSamplerAddressMode to_vk_address_mode(AddressMode m);
VkSamplerMipmapMode to_vk_mipmap_mode(MipMapMode m);
VkDescriptorType to_vk_descriptor_type(DescriptorType t);
VkImageViewType to_vk_view_type(ViewDimension dim);
VkImageAspectFlags to_vk_image_aspect(ImageAspect aspect);
VkImageAspectFlags aspect_for_format(Format fmt);

} // namespace nf::rhi
