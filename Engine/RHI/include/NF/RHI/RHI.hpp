#pragma once

// NF/RHI/RHI.hpp — Rendering Hardware Interface abstraction
// Design doc Section 7: IGraphicsDevice with Vulkan/D3D12/Metal backends.
// This is the pure interface layer — no API specifics here.

#include <NF/Core/Types.hpp>  // brings in u32/usize + NonCopyable/NonMovable

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace nf::rhi {

// --- Enums ---

enum class Format : u8 {
    Unknown = 0,

    // Color
    R8_UNorm,
    R8G8_UNorm,
    R8G8B8A8_UNorm,
    R8G8B8A8_sRGB,
    B8G8R8A8_UNorm,
    R16G16B16A16_SFloat,
    R32_SFloat,
    R32G32_SFloat,
    R32G32B32_SFloat,
    R32G32B32A32_SFloat,

    // Depth/Stencil
    D16_UNorm,
    D32_SFloat,
    D24_UNorm_S8_UInt,
    D32_SFloat_S8_UInt,

    // Compressed
    BC1_RGB_UNorm,
    BC3_UNorm,
    BC5_UNorm,
    BC7_UNorm,
};

enum class ImageUsage : u16 {
    None        = 0,
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    Sampled     = 1 << 2,
    Storage     = 1 << 3,
    ColorAtt    = 1 << 4,
    DepthAtt   = 1 << 5,
    InputAtt    = 1 << 6,
};
inline ImageUsage operator|(ImageUsage a, ImageUsage b) {
    return static_cast<ImageUsage>(static_cast<u16>(a) | static_cast<u16>(b));
}
inline bool has_usage(ImageUsage a, ImageUsage test) {
    return (static_cast<u16>(a) & static_cast<u16>(test)) != 0;
}

enum class BufferUsage : u16 {
    None       = 0,
    Vertex     = 1 << 0,
    Index      = 1 << 1,
    Uniform    = 1 << 2,
    Storage    = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u16>(a) | static_cast<u16>(b));
}

enum class MemoryUsage : u8 {
    GPUOnly  = 0,
    CPUOnly  = 1,
    CPUToGPU = 2, // Staging
    GPUToCPU = 3, // Readback
};

enum class PresentMode : u8 {
    Immediate  = 0,
    FIFO        = 1, // VSync
    Mailbox     = 2, // Triple buffer
};

enum class PrimitiveTopology : u8 {
    PointList = 0,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

enum class CullMode : u8 {
    None = 0,
    Front,
    Back,
};

enum class FrontFace : u8 {
    CCW = 0,
    CW = 1,
};

enum class CompareOp : u8 {
    Never = 0,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class BlendOp : u8 {
    Add = 0,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

enum class BlendFactor : u8 {
    Zero = 0,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
};

enum class ShaderStage : u16 {
    Vertex   = 1 << 0,
    Fragment = 1 << 1,
    Compute  = 1 << 2,
    Geometry = 1 << 3,
};
inline ShaderStage operator|(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<u16>(a) | static_cast<u16>(b));
}

// --- Create Info structs ---

struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUsage memory = MemoryUsage::GPUOnly;
};

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    u32 mip_levels = 1;
    u32 array_layers = 1;
    Format format = Format::Unknown;
    ImageUsage usage = ImageUsage::None;
};

struct SwapchainDesc {
    u32 width = 0;
    u32 height = 0;
    Format format = Format::B8G8R8A8_UNorm;
    PresentMode present = PresentMode::FIFO;
    u32 image_count = 2;
};

// --- Texture view dimension / aspect ---

enum class ViewDimension : u8 {
    View2D = 0,
    View2DArray,
    Cube,
    CubeArray,
    View3D,
};

enum class ImageAspect : u8 {
    Color = 0,
    Depth,
    Stencil,
    DepthStencil,
};

/// Describes how a TextureView interprets its underlying Texture.
///
/// The default (View2D, Color, mip 0, layer 0) preserves the legacy
/// `create_texture_view(texture)` behaviour so existing code needs no
/// change. Future paths (cube maps, arrays, mip chains, depth/stencil)
/// are expressed by filling the extra fields — the backend maps them to
/// the appropriate VkImageViewType / aspectMask.
struct TextureViewDesc {
    const class Texture* texture = nullptr; // required, non-owning
    ViewDimension dimension = ViewDimension::View2D;
    ImageAspect aspect = ImageAspect::Color;
    Format format = Format::Unknown; // Unknown = use texture's format
    u32 base_mip = 0;
    u32 mip_count = 1;
    u32 base_layer = 0;
    u32 layer_count = 1;
};

struct ShaderModuleDesc {
    std::span<const u8> code;
    ShaderStage stage = ShaderStage::Vertex;
};

struct VertexAttrib {
    u32 location = 0;
    u32 offset = 0;
    Format format = Format::Unknown;
};

struct VertexLayout {
    u32 binding = 0;
    u32 stride = 0;
    std::span<const VertexAttrib> attributes;
};

struct RasterizerState {
    CullMode cull_mode = CullMode::Back;
    FrontFace front_face = FrontFace::CCW;
    bool wireframe = false;
};

struct DepthState {
    bool test_enabled = true;
    bool write_enabled = true;
    CompareOp compare = CompareOp::Less;
};

struct ColorAttachment {
    Format format = Format::B8G8R8A8_UNorm;
    bool blend_enabled = false;
    BlendOp color_blend_op = BlendOp::Add;
    BlendFactor src_color = BlendFactor::SrcAlpha;
    BlendFactor dst_color = BlendFactor::OneMinusSrcAlpha;
};

struct RenderPassDesc {
    std::span<const ColorAttachment> color_attachments;
    Format depth_format = Format::Unknown;
    bool has_depth = false;

    /// What happens to the depth attachment when the pass begins. Clear
    /// (default) suits the first depth-writing pass; Load is required for a
    /// second pass that shares the depth buffer written by a prepass — with
    /// depth writes disabled there, a Clear would silently wipe the prepass
    /// output and every downstream depth read would see 1.0.
    enum class DepthLoad : u8 { Clear = 0, Load };
    DepthLoad depth_load = DepthLoad::Clear;

    /// What happens to the color attachments when the pass begins. Clear
    /// (default) is the common case; Load preserves whatever a previous pass
    /// in the same command buffer wrote — e.g. the editor UI overlay pass
    /// drawing ImGui over the already-rendered scene. With Load, initial
    /// layout follows present_source exactly like the final layout does, so
    /// chaining passes on the same image stays validation-clean.
    enum class ColorLoad : u8 { Clear = 0, Load };
    ColorLoad color_load = ColorLoad::Clear;

    /// True when the color attachments are swapchain images that will be
    /// presented. It decides the attachments' layout when the pass ends:
    /// presentation-ready for on-screen passes, renderable/sampleable for
    /// offscreen ones. Getting this wrong is silent on some drivers and a
    /// validation error on others, so it is explicit rather than inferred.
    bool present_source = false;
};

/// One RGBA clear value per color attachment. Modelled as a real type rather
/// than a flat float array so a "wrong number of floats" mistake cannot slip
/// through as a silently wrong clear colour.
struct ClearValue {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// --- Sampler filter / addressing enums ---

enum class Filter : u8 {
    Nearest = 0,
    Linear,
};

enum class AddressMode : u8 {
    Repeat = 0,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

enum class MipMapMode : u8 {
    None = 0,    // No mip sampling — uses level 0 only
    Nearest,
    Linear,
};

/// How a texture is sampled: filter, wrap, mip, anisotropy.
///
/// Mirrors VkSamplerCreateInfo in a backend-neutral way. D3D12 and Metal map
/// these to their own static sampler descriptions.
struct SamplerDesc {
    Filter mag = Filter::Linear;
    Filter min = Filter::Linear;
    MipMapMode mip = MipMapMode::None;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    AddressMode address_w = AddressMode::Repeat;
    bool anisotropy_enable = false;
    float max_anisotropy = 1.0f;
    float min_lod = 0.0f;
    float max_lod = 0.0f;
};

// --- Descriptor set abstraction ---
//
// The RHI uses a "binding" model inspired by Vulkan's descriptor sets but
// without exposing Vulkan types. The flow is:
//
//   1. Declare a DescriptorSetLayout (one per shader/pipeline configuration).
//      Each binding has a type (UniformBuffer, SampledImage, Sampler), a
//      shader stage visibility, and a binding index.
//   2. Allocate a DescriptorSet from a pool (owned by the device).
//   3. Write resources into the set: bind a Buffer, a TextureView, or a Sampler.
//   4. Bind the set in a command buffer before drawing.
//
// This is the minimal surface that a renderer needs for per-draw material data
// and textures. Storage buffers and push descriptors are deferred until the
// compute pipeline or the bindless path lands.

enum class DescriptorType : u8 {
    UniformBuffer = 0,
    SampledImage  = 1,  // Combined image + sampler (Vulkan: COMBINED_IMAGE_SAMPLER)
    Sampler        = 2,  // Separate sampler (Vulkan: SAMPLER)
    SampledImageSeparate = 3, // Texture view bound without a sampler (Vulkan: SAMPLED_IMAGE)
};

struct DescriptorBinding {
    u32 binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    ShaderStage stages = ShaderStage::Vertex | ShaderStage::Fragment;
    u32 count = 1; // Array size (1 for non-array bindings)
};

struct DescriptorSetLayoutDesc {
    std::span<const DescriptorBinding> bindings;
};

/// A write to a descriptor set. One write per binding slot.
struct DescriptorWrite {
    u32 binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;

    /// For UniformBuffer bindings. One buffer + offset + range per write.
    const class Buffer* buffer = nullptr;
    usize buffer_offset = 0;
    usize buffer_range = 0; // 0 = whole buffer (WHOLE_SIZE)

    /// For SampledImage / SampledImageSeparate bindings.
    /// A combined image+sampler uses both view and sampler.
    const class TextureView* texture_view = nullptr;
    const class Sampler* sampler = nullptr;
};

struct PipelineDesc {
    ShaderStage stages = ShaderStage::Vertex;
    const class ShaderModule* vs = nullptr;
    const class ShaderModule* fs = nullptr;
    VertexLayout vertex_layout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterizerState rasterizer;
    DepthState depth;

    /// The render pass this pipeline will be used with. Required.
    ///
    /// A pointer, not a description: Vulkan needs a VkRenderPass handle at
    /// pipeline-creation time, and if the pipeline carried its own copy of the
    /// description, the two would have to be kept in sync by hand. Backends
    /// that only need the formats (D3D12, Metal) read them from
    /// `render_pass->desc()`.
    const class RenderPass* render_pass = nullptr;

    /// The descriptor set layout this pipeline uses. Optional — push
    /// constants and hardcoded vertices can draw without any descriptors.
    const class DescriptorSetLayout* descriptor_set_layout = nullptr;

    /// Size in bytes of the push-constant block exposed to the shaders.
    /// Push constants are the cheap path for small per-draw data (transform,
    /// material index) and avoid descriptor-set churn entirely.
    u32 push_constant_size = 0;
    ShaderStage push_constant_stages = ShaderStage::Vertex;
};

// --- Handle types ---
struct BufferHandle    { u32 id = u32_max; bool valid() const { return id != u32_max; } };
struct TextureHandle   { u32 id = u32_max; bool valid() const { return id != u32_max; } };
struct PipelineHandle  { u32 id = u32_max; bool valid() const { return id != u32_max; } };
struct ShaderModuleHandle { u32 id = u32_max; bool valid() const { return id != u32_max; } };
struct RenderPassHandle  { u32 id = u32_max; bool valid() const { return id != u32_max; } };
struct FramebufferHandle { u32 id = u32_max; bool valid() const { return id != u32_max; } };

// --- Forward declarations ---
class Buffer;
class Texture;
class TextureView;
class ShaderModule;
class Pipeline;
class RenderPass;
class Framebuffer;
class CommandBuffer;
class Sampler;
class DescriptorSetLayout;
class DescriptorSet;
class DescriptorAllocator;
class UploadContext;

struct DeviceDesc {
    void* window_handle = nullptr;
    bool enable_validation = false;
};

// --- Buffer ---
class Buffer {
public:
    virtual ~Buffer() = default;
    virtual void* map() = 0;
    virtual void unmap() = 0;
    virtual void update(const void* data, usize offset, usize size) = 0;
    virtual usize size() const = 0;
};

// --- Texture ---
class Texture {
public:
    virtual ~Texture() = default;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    virtual Format format() const = 0;

    /// Backend-assigned creation serial, unique per texture object for the
    /// process lifetime (never reused, unlike raw pointers). Lets caches
    /// (e.g. Renderer3D's per-target framebuffers) key on identity without
    /// dangling on allocator address reuse. Backends without the concept
    /// return 0.
    virtual u64 creation_serial() const { return 0; }
};

// --- TextureView ---
//
// A view into a texture for a specific aspect and mip range. In Vulkan this is
// a VkImageView. The texture itself owns the backing image; the view owns only
// the view handle, so it can outlive individual render passes but must not
// outlive the texture it references.
class TextureView {
public:
    virtual ~TextureView() = default;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    virtual Format format() const = 0;
};

// --- ShaderModule ---
class ShaderModule {
public:
    virtual ~ShaderModule() = default;
    virtual ShaderStage stage() const = 0;
};

// --- Pipeline ---
class Pipeline {
public:
    virtual ~Pipeline() = default;
};

// --- RenderPass ---
class RenderPass {
public:
    virtual ~RenderPass() = default;

    /// The description this pass was built from. Returned by value: the span
    /// inside points at storage owned by the RenderPass, so it stays valid as
    /// long as the RenderPass object does — but not longer.
    virtual RenderPassDesc desc() const = 0;
    virtual u32 color_attachment_count() const = 0;
    virtual bool has_depth() const = 0;
};

// --- Framebuffer ---
class Framebuffer {
public:
    virtual ~Framebuffer() = default;
};

// --- Sampler ---
//
// Immutable sampler state. In Vulkan this is a VkSampler, created once and
// reused. D3D12 maps it to a static sampler in the root signature; Metal maps
// it to an MTLSamplerState.
class Sampler {
public:
    virtual ~Sampler() = default;
};

// --- DescriptorSetLayout ---
//
// Describes the layout of a descriptor set: which bindings, what types, which
// shader stages. In Vulkan this is VkDescriptorSetLayout. The pipeline is
// built against it, so it must outlive the pipeline.
class DescriptorSetLayout {
public:
    virtual ~DescriptorSetLayout() = default;
};

// --- DescriptorSet ---
//
// A concrete descriptor set allocated from the device's descriptor pool. It
// holds the actual bindings (buffer pointers, texture views, samplers) that
// the shader reads. Write once, bind many.
class DescriptorSet {
public:
    virtual ~DescriptorSet() = default;
};

// --- DescriptorAllocator ---
//
// Growable per-frame allocator that replaces the old `maxSets = 1` per-set pool.
// A Renderer creates one allocator per frame (or per thread) and uses it to
// allocate many sets of any layout without knowing the exact pool sizes in
// advance. The pool sizes are always derived from the actual layout bindings
// (one entry per descriptor type, count = binding.count * sets_per_pool), so
// there is no hard-coded random sizing.
//
// Frame lifecycle:
//   allocator->allocate(layout)  // may grow (new VkDescriptorPool)
//   ... bind / draw ...
//   allocator->reset()           // recycles all pools (no destroy/create churn)
class DescriptorAllocator {
public:
    virtual ~DescriptorAllocator() = default;
    virtual std::unique_ptr<DescriptorSet> allocate(const DescriptorSetLayout& layout) = 0;
    virtual void reset() = 0;
    virtual u32 allocated_count() const = 0;
    virtual u32 pool_count() const = 0;
};

// --- Synchronization primitives ---
//
// GPU work is asynchronous, so the RHI exposes semaphores (GPU-to-GPU ordering)
// and fences (GPU-to-CPU completion) explicitly. Hiding these inside the
// swapchain would make multi-queue submission (async compute, transfer) and
// explicit frame pipelining impossible later.

class Semaphore {
public:
    virtual ~Semaphore() = default;
};

class Fence {
public:
    virtual ~Fence() = default;

    /// Blocks until the fence is signaled. Pass u64_max to wait forever.
    virtual bool wait(u64 timeout_ns = u64_max) = 0;
    virtual void reset() = 0;
    virtual bool is_signaled() = 0;
};

// --- UploadContext ---
//
// Batched transfer helper that keeps staging buffers alive until the GPU is
// done. The synchronous `Buffer::update()` path (init-time) remains for
// convenience, but per-frame uploads must go through here so the RHI never
// does `vkQueueWaitIdle` in the hot path.
class UploadContext {
public:
    virtual ~UploadContext() = default;
    virtual void copy_buffer(const Buffer& src, Buffer& dst,
                             usize src_offset, usize dst_offset, usize size) = 0;
    virtual void copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                        usize buffer_offset,
                                        u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h) = 0;
    virtual std::unique_ptr<Fence> submit() = 0;
};

/// Where in the pipeline a semaphore wait blocks. Required for correctness and
/// performance: waiting on the swapchain acquire at ColorAttachmentOutput lets
/// vertex shading overlap with presentation, which a blanket "wait for
/// everything" would forbid.
enum class PipelineStage : u32 {
    TopOfPipe = 0,
    VertexInput,
    VertexShader,
    FragmentShader,
    ColorAttachmentOutput,
    ComputeShader,
    Transfer,
    BottomOfPipe,
    AllCommands,
};

struct SubmitInfo {
    std::span<const Semaphore* const> wait_semaphores;
    /// One stage per wait semaphore. When empty, every wait uses
    /// PipelineStage::AllCommands.
    std::span<const PipelineStage> wait_stages;
    std::span<const Semaphore* const> signal_semaphores;
    Fence* signal_fence = nullptr; // Signaled when the submission completes
};

// --- CommandBuffer ---
//
// Command buffers are owned and reused by the caller, not by the device. The
// GPU reads them asynchronously, so destroying or re-recording one while it is
// still in flight is undefined behaviour — wait on the submission fence first.
class CommandBuffer {
public:
    virtual ~CommandBuffer() = default;

    /// Puts the buffer back into the recordable state. Only legal once the
    /// GPU has finished executing it.
    virtual void reset() = 0;
    virtual void begin() = 0;
    virtual void end() = 0;

    /// `clear_values` holds one entry per color attachment. Fewer entries than
    /// attachments is allowed — the remainder clear to opaque black.
    virtual void begin_render_pass(const RenderPass& pass, const Framebuffer& fb,
                                    std::span<const ClearValue> clear_values,
                                    float clear_depth = 1.0f, u8 clear_stencil = 0) = 0;
    virtual void end_render_pass() = 0;

    virtual void bind_pipeline(const Pipeline& pipeline) = 0;
    virtual void bind_vertex_buffers(std::span<const Buffer* const> buffers) = 0;
    virtual void bind_index_buffer(const Buffer& buffer, usize offset = 0) = 0;

    /// Binds descriptor sets at `first_set`. All sets must share the layout
    /// the pipeline was created with.
    virtual void bind_descriptor_sets(const DescriptorSetLayout& layout,
                                     std::span<const DescriptorSet* const> sets,
                                     u32 first_set = 0) = 0;

    /// Pushes a small block of constants to the shaders. The pipeline must
    /// have been created with `push_constant_size > 0` and matching stages.
    virtual void push_constants(ShaderStage stages, u32 offset, u32 size,
                                const void* data) = 0;

    virtual void set_viewport(u32 x, u32 y, u32 width, u32 height,
                               float min_depth = 0.0f, float max_depth = 1.0f) = 0;
    virtual void set_scissor(u32 x, u32 y, u32 width, u32 height) = 0;

    virtual void draw(u32 vertex_count, u32 instance_count = 1,
                      u32 first_vertex = 0, u32 first_instance = 0) = 0;
    virtual void draw_indexed(u32 index_count, u32 instance_count = 1,
                              u32 first_index = 0, i32 vertex_offset = 0,
                              u32 first_instance = 0) = 0;

    virtual void copy_buffer(const Buffer& src, Buffer& dst,
                             usize src_offset, usize dst_offset, usize size) = 0;
    virtual void copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                         usize buffer_offset,
                                         u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h) = 0;

    /// Ends the upload chain: makes a texture readable by shaders.
    ///
    /// After copy_buffer_to_texture() the image sits in the transfer-destination
    /// layout, which a sampler is not allowed to read. Call this once the
    /// upload is done to complete
    ///   Undefined → TransferDst → ShaderRead.
    ///
    /// It is a separate command rather than something copy_buffer_to_texture()
    /// does automatically because a texture can be uploaded in several pieces
    /// (mips, array layers) and transitioning after every piece would be pure
    /// overhead.
    virtual void transition_texture_for_sampling(Texture& texture) = 0;

    /// Generates a full mip chain with linear blits. Contract: mip level 0
    /// holds valid data in TRANSFER_DST layout (i.e. call immediately after
    /// copy_buffer_to_texture, before transition_texture_for_sampling) and
    /// every other level is UNDEFINED. On success every level ends in
    /// SHADER_READ layout (so the transition call becomes a no-op) and the
    /// function returns true. Textures with a single mip level succeed
    /// trivially. Returns false (logs) when the format cannot be blitted —
    /// the caller must then keep single-mip sampling.
    virtual bool generate_mipmaps(Texture& texture) = 0;

    // Generic barrier: transitions a texture from one usage to another.
    // This is the foundation for RenderGraph's automatic barrier inference.
    // It replaces the hardcoded TRANSFER_DST → SHADER_READ etc. with a generic
    // state machine that can handle Depth, GBuffer, Lighting, PostProcess without
    // per-feature transition code.
    virtual void barrier_texture(Texture& texture, ImageUsage before, ImageUsage after) = 0;

    /// GPU → CPU readback: copies a texture region into a buffer. Required for
    /// screenshots, GPU-side occlusion culling, and — critically — for tests
    /// that must prove pixels were actually written.
    /// The destination must be host-visible (MemoryUsage::GPUToCPU).
    virtual void copy_texture_to_buffer(const Texture& src, Buffer& dst,
                                         u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h,
                                         usize buffer_offset = 0) = 0;
};

// --- Swapchain ---
class Swapchain {
public:
    virtual ~Swapchain() = default;

    /// Acquires the next presentable image and signals `signal_semaphore`
    /// once the image is actually available to the GPU.
    /// Returns u32_max if the swapchain must be recreated (resize / lost).
    virtual u32 acquire_next_image(const Semaphore& signal_semaphore) = 0;

    /// Queues presentation, waiting on `wait_semaphores` (normally the
    /// render-complete semaphore signaled by the final submit).
    virtual void present(u32 image_index, std::span<const Semaphore* const> wait_semaphores) = 0;

    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    virtual Format format() const = 0;
    virtual u32 image_count() const = 0;
    virtual Texture* get_texture(u32 index) = 0;
};

// --- IGraphicsDevice ---
class IGraphicsDevice {
public:
    virtual ~IGraphicsDevice() = default;

    // Factory
    virtual bool init(void* native_window_handle) = 0;
    virtual bool init(const DeviceDesc& desc) = 0;
    virtual void shutdown() = 0;

    virtual std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) = 0;
    virtual std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> create_texture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<TextureView> create_texture_view(const Texture& texture) = 0;
    virtual std::unique_ptr<TextureView> create_texture_view(const TextureViewDesc& desc) = 0;
    virtual std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> create_pipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<RenderPass> create_render_pass(const RenderPassDesc& desc) = 0;
    virtual std::unique_ptr<Framebuffer> create_framebuffer(const RenderPass& pass,
                                                              std::span<Texture* const> color_attachments,
                                                              Texture* depth_attachment = nullptr) = 0;

    virtual std::unique_ptr<Sampler> create_sampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorSetLayout> create_descriptor_set_layout(
        const DescriptorSetLayoutDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorSet> create_descriptor_set(
        const DescriptorSetLayout& layout) = 0;
    virtual std::unique_ptr<DescriptorAllocator> create_descriptor_allocator(u32 sets_per_pool = 32) = 0;
    virtual std::unique_ptr<UploadContext> create_upload_context() = 0;

    /// Writes resources into an existing descriptor set. The writes are
    /// applied immediately (the backend may batch internally, but the
    /// descriptor set is valid to bind as soon as this returns).
    virtual void update_descriptor_set(const DescriptorSet& set,
                                      std::span<const DescriptorWrite> writes) = 0;

    virtual std::unique_ptr<CommandBuffer> create_command_buffer() = 0;
    virtual std::unique_ptr<Semaphore> create_semaphore() = 0;
    virtual std::unique_ptr<Fence> create_fence(bool signaled = false) = 0;

    /// Submits a recorded command buffer to the graphics queue.
    /// `cmd` must stay alive until `info.signal_fence` is signaled.
    virtual void submit(const CommandBuffer& cmd, const SubmitInfo& info = {}) = 0;
    virtual void wait_idle() = 0;

    virtual const char* backend_name() const = 0;

    /// True when the backend's validation machinery is actually active (the
    /// KHRONOS validation layer was found and enabled). Tests use this to
    /// decide whether a "validation clean" assertion is enforceable.
    virtual bool validation_enabled() const = 0;

    /// Number of RHI objects (buffers, textures, views, pipelines, ...) created
    /// through this device that have not been destroyed yet. Tests use this to
    /// prove resource lifetime correctness: create everything, drop it, and
    /// the count must return to zero before shutdown().
    virtual u32 alive_objects() const = 0;
};

/// Global Vulkan validation-error counter, incremented by the debug messenger
/// for every VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR message. Always 0 when
/// validation is disabled (no layers). Tests snapshot it around a render and
/// assert the delta is zero — this is what "Vulkan Validation: CLEAN" means.
u32 validation_error_count();
void reset_validation_error_count();

/// Internal hook: called by backends when their validation machinery reports
/// an error. Declared here (rather than in a backend header) because the
/// counter itself is backend-neutral state shared across device instances.
void record_validation_error();

// --- Factory function ---
std::unique_ptr<IGraphicsDevice> create_device();

} // namespace nf::rhi
