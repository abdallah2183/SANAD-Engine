// NF/RHI/src/Vulkan/Device_Vk.cpp — Vulkan device implementation

#include <NF/RHI/Vulkan/Device_Vk.hpp>

#include "VulkanLoader.hpp"
#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <cstring>
#include <set>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace nf::rhi {

// --- Format / enum conversion ---------------------------------------------

VkFormat to_vk_format(Format fmt) {
    switch (fmt) {
        case Format::R8_UNorm:            return VK_FORMAT_R8_UNORM;
        case Format::R8G8_UNorm:          return VK_FORMAT_R8G8_UNORM;
        case Format::R8G8B8A8_UNorm:      return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8_sRGB:       return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::B8G8R8A8_UNorm:      return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::R16G16B16A16_SFloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32_SFloat:          return VK_FORMAT_R32_SFLOAT;
        case Format::R32G32_SFloat:       return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32G32B32_SFloat:    return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::R32G32B32A32_SFloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::D16_UNorm:           return VK_FORMAT_D16_UNORM;
        case Format::D32_SFloat:          return VK_FORMAT_D32_SFLOAT;
        case Format::D24_UNorm_S8_UInt:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_SFloat_S8_UInt:  return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case Format::BC1_RGB_UNorm:       return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case Format::BC3_UNorm:           return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC5_UNorm:           return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::BC7_UNorm:           return VK_FORMAT_BC7_UNORM_BLOCK;
        default:                          return VK_FORMAT_UNDEFINED;
    }
}

Format from_vk_format(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8_UNORM:              return Format::R8_UNorm;
        case VK_FORMAT_R8G8_UNORM:            return Format::R8G8_UNorm;
        case VK_FORMAT_R8G8B8A8_UNORM:        return Format::R8G8B8A8_UNorm;
        case VK_FORMAT_R8G8B8A8_SRGB:         return Format::R8G8B8A8_sRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:        return Format::B8G8R8A8_UNorm;
        case VK_FORMAT_R16G16B16A16_SFLOAT:   return Format::R16G16B16A16_SFloat;
        case VK_FORMAT_R32_SFLOAT:            return Format::R32_SFloat;
        case VK_FORMAT_R32G32_SFLOAT:         return Format::R32G32_SFloat;
        case VK_FORMAT_R32G32B32_SFLOAT:      return Format::R32G32B32_SFloat;
        case VK_FORMAT_R32G32B32A32_SFLOAT:   return Format::R32G32B32A32_SFloat;
        case VK_FORMAT_D16_UNORM:             return Format::D16_UNorm;
        case VK_FORMAT_D32_SFLOAT:            return Format::D32_SFloat;
        case VK_FORMAT_D24_UNORM_S8_UINT:     return Format::D24_UNorm_S8_UInt;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:    return Format::D32_SFloat_S8_UInt;
        default:                              return Format::Unknown;
    }
}

VkShaderStageFlagBits to_vk_shader_stage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        default:                    return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

VkPrimitiveTopology to_vk_topology(PrimitiveTopology topo) {
    switch (topo) {
        case PrimitiveTopology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default:                               return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkCullModeFlags to_vk_cull_mode(CullMode mode) {
    switch (mode) {
        case CullMode::None:  return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
        default:              return VK_CULL_MODE_BACK_BIT;
    }
}

VkFrontFace to_vk_front_face(FrontFace ff) {
    return ff == FrontFace::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                : VK_FRONT_FACE_CLOCKWISE;
}

VkCompareOp to_vk_compare_op(CompareOp op) {
    switch (op) {
        case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:         return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
        default:                      return VK_COMPARE_OP_LESS;
    }
}

VkBlendOp to_vk_blend_op(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:             return VK_BLEND_OP_MIN;
        case BlendOp::Max:             return VK_BLEND_OP_MAX;
        default:                       return VK_BLEND_OP_ADD;
    }
}

VkBlendFactor to_vk_blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:                            return VK_BLEND_FACTOR_ONE;
    }
}

VkFilter to_vk_filter(Filter f) {
    switch (f) {
        case Filter::Nearest: return VK_FILTER_NEAREST;
        case Filter::Linear:  return VK_FILTER_LINEAR;
        default:               return VK_FILTER_LINEAR;
    }
}

VkSamplerAddressMode to_vk_address_mode(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:                          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkSamplerMipmapMode to_vk_mipmap_mode(MipMapMode m) {
    switch (m) {
        case MipMapMode::None:    return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case MipMapMode::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case MipMapMode::Linear:  return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        default:                   return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

VkDescriptorType to_vk_descriptor_type(DescriptorType t) {
    switch (t) {
        case DescriptorType::UniformBuffer:           return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::SampledImage:             return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::Sampler:                  return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::SampledImageSeparate:    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        default:                                        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

VkImageViewType to_vk_view_type(ViewDimension dim) {
    switch (dim) {
        case ViewDimension::View2D:      return VK_IMAGE_VIEW_TYPE_2D;
        case ViewDimension::View2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case ViewDimension::Cube:        return VK_IMAGE_VIEW_TYPE_CUBE;
        case ViewDimension::CubeArray:   return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case ViewDimension::View3D:      return VK_IMAGE_VIEW_TYPE_3D;
        default:                          return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkImageAspectFlags to_vk_image_aspect(ImageAspect aspect) {
    switch (aspect) {
        case ImageAspect::Color:        return VK_IMAGE_ASPECT_COLOR_BIT;
        case ImageAspect::Depth:        return VK_IMAGE_ASPECT_DEPTH_BIT;
        case ImageAspect::Stencil:      return VK_IMAGE_ASPECT_STENCIL_BIT;
        case ImageAspect::DepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:                         return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkImageAspectFlags aspect_for_format(Format fmt) {
    switch (fmt) {
        case Format::D16_UNorm:
        case Format::D32_SFloat:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case Format::D24_UNorm_S8_UInt:
        case Format::D32_SFloat_S8_UInt:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkImageUsageFlags to_vk_image_usage(ImageUsage usage) {
    VkImageUsageFlags flags = 0;
    if (has_usage(usage, ImageUsage::TransferSrc)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_usage(usage, ImageUsage::TransferDst)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (has_usage(usage, ImageUsage::Sampled))     flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_usage(usage, ImageUsage::Storage))     flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_usage(usage, ImageUsage::ColorAtt))    flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_usage(usage, ImageUsage::DepthAtt))    flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_usage(usage, ImageUsage::InputAtt))    flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return flags;
}

VkBufferUsageFlags to_vk_buffer_usage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    const auto has = [](BufferUsage u, BufferUsage test) {
        return (static_cast<u16>(u) & static_cast<u16>(test)) != 0;
    };
    if (has(usage, BufferUsage::Vertex))      flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has(usage, BufferUsage::Index))       flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has(usage, BufferUsage::Uniform))     flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has(usage, BufferUsage::Storage))     flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has(usage, BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has(usage, BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

static VkPipelineStageFlags to_vk_stage(PipelineStage stage) {
    switch (stage) {
        case PipelineStage::TopOfPipe:            return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        case PipelineStage::VertexInput:          return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        case PipelineStage::VertexShader:         return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        case PipelineStage::FragmentShader:       return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        case PipelineStage::ColorAttachmentOutput:return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        case PipelineStage::ComputeShader:        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case PipelineStage::Transfer:             return VK_PIPELINE_STAGE_TRANSFER_BIT;
        case PipelineStage::BottomOfPipe:         return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        case PipelineStage::AllCommands:          return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        default:                                  return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

VkPresentModeKHR to_vk_present_mode(PresentMode mode) {
    switch (mode) {
        case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PresentMode::FIFO:      return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::Mailbox:   return VK_PRESENT_MODE_MAILBOX_KHR;
        default:                     return VK_PRESENT_MODE_FIFO_KHR;
    }
}

// --- VulkanDevice ---------------------------------------------------------

VulkanDevice::VulkanDevice() {
    if (!load_vulkan_loader()) {
        NF_LOG_WARN(LogCategory::RHI,
            "Vulkan loader unavailable — device will fail to initialize");
    }
}

VulkanDevice::~VulkanDevice() {
    shutdown();
}

bool VulkanDevice::init(void* native_window_handle) {
    return init(DeviceDesc{ native_window_handle, false });
}

bool VulkanDevice::init(const DeviceDesc& desc) {
    m_requested_validation = desc.enable_validation;

    if (!is_vulkan_loader_loaded()) {
        NF_LOG_ERROR(LogCategory::RHI, "Vulkan loader is not available");
        return false;
    }

    if (!create_instance())            return false;
    if (!load_instance_functions(m_ctx.instance)) return false;
    if (!setup_debug_messenger())      return false;

    // A null window handle means "headless": no surface, no presentation.
    // Offscreen rendering, compute and readback all work without one, and GPU
    // tests should not have to open a window just to prove a draw is correct.
    if (desc.window_handle) {
        if (!create_surface(desc.window_handle)) return false;
    } else {
        NF_LOG_DEBUG(LogCategory::RHI, "Headless device: no surface will be created");
    }

    if (!pick_physical_device())       return false;
    // create_logical_device() loads the device-level entry points itself:
    // vkGetDeviceQueue is a device function and is needed to finish device
    // setup, so loading cannot be deferred until after it returns.
    if (!create_logical_device())      return false;
    if (!create_command_pool())        return false;

    NF_LOG_INFO(LogCategory::RHI, "Vulkan device initialized [{}] (validation: {})",
                backend_name(), m_ctx.enable_validation ? "on" : "off");
    return true;
}

void VulkanDevice::shutdown() {
    if (!m_ctx.instance) {
        return; // Not initialized (or already shut down)
    }

    // Teardown must tolerate a half-built device: init can fail after
    // vkCreateDevice but before the device-level entry points are resolvable,
    // and a crash during cleanup would hide the real error.
    const bool device_usable = (m_ctx.device != VK_NULL_HANDLE) &&
                               (vkDeviceWaitIdle != nullptr) &&
                               (vkDestroyDevice != nullptr);

    if (device_usable) {
        vkDeviceWaitIdle(m_ctx.device);
    }

    if (m_ctx.command_pool && device_usable && vkDestroyCommandPool) {
        vkDestroyCommandPool(m_ctx.device, m_ctx.command_pool, nullptr);
        m_ctx.command_pool = VK_NULL_HANDLE;
    }

    if (device_usable) {
        vkDestroyDevice(m_ctx.device, nullptr);
        m_ctx.device = VK_NULL_HANDLE;
    }

    if (m_ctx.surface) {
        vkDestroySurfaceKHR(m_ctx.instance, m_ctx.surface, nullptr);
        m_ctx.surface = VK_NULL_HANDLE;
    }

    if (m_ctx.debug_messenger && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(m_ctx.instance, m_ctx.debug_messenger, nullptr);
        m_ctx.debug_messenger = VK_NULL_HANDLE;
    }

    if (vkDestroyInstance) {
        vkDestroyInstance(m_ctx.instance, nullptr);
        m_ctx.instance = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "NOVAForge Engine";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "NOVAForge";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    // --- Discover what the installed loader actually supports -------------
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(layer_count);
    if (layer_count > 0) {
        vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
    }

    u32 ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available_exts(ext_count);
    if (ext_count > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, available_exts.data());
    }

    const auto has_layer = [&](const char* name) {
        for (const auto& l : available_layers) {
            if (std::strcmp(l.layerName, name) == 0) return true;
        }
        return false;
    };
    const auto has_ext = [&](const char* name) {
        for (const auto& e : available_exts) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };

    std::vector<const char*> layers;

    bool want_validation = m_requested_validation;
#ifndef NDEBUG
    want_validation = true; // Debug always enables validation when available
#endif
    // In Release, validation is only enabled when explicitly requested via
    // DeviceDesc::enable_validation (wired to --validation).

    bool debug_utils_available = has_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    if (want_validation && has_layer("VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        m_ctx.enable_validation = true;
        NF_LOG_INFO(LogCategory::RHI, "Vulkan validation layer enabled");
    } else {
        m_ctx.enable_validation = false;
        if (m_requested_validation && want_validation) {
            NF_LOG_WARN(LogCategory::RHI,
                "Vulkan validation requested but VK_LAYER_KHRONOS_validation not present");
        }
    }

    if (debug_utils_available) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        NF_LOG_DEBUG(LogCategory::RHI,
            "VK_EXT_debug_utils unavailable — debug markers disabled");
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount = static_cast<u32>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();

    if (!NF_VK_CHECK(vkCreateInstance(&create_info, nullptr, &m_ctx.instance))) {
        return false;
    }

    NF_LOG_INFO(LogCategory::RHI, "Vulkan instance created");
    return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data) {

    (void)type;
    (void)user_data;

    const char* msg = data->pMessage ? data->pMessage : "(null)";

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        record_validation_error();
        NF_LOG_ERROR(LogCategory::RHI, "[Vulkan] {}", msg);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        NF_LOG_WARN(LogCategory::RHI, "[Vulkan] {}", msg);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        NF_LOG_DEBUG(LogCategory::RHI, "[Vulkan] {}", msg);
    }

    return VK_FALSE; // Never abort the calling command
}

bool VulkanDevice::setup_debug_messenger() {
    if (!vkCreateDebugUtilsMessengerEXT) {
        return true; // Extension not present — nothing to set up
    }

    VkDebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = debug_callback;

    if (!NF_VK_CHECK(vkCreateDebugUtilsMessengerEXT(
            m_ctx.instance, &create_info, nullptr, &m_ctx.debug_messenger))) {
        NF_LOG_WARN(LogCategory::RHI, "Failed to create debug messenger");
        return true; // Non-fatal
    }

    return true;
}

bool VulkanDevice::create_surface(void* hwnd) {
#ifdef _WIN32
    if (!hwnd) {
        NF_LOG_ERROR(LogCategory::RHI, "create_surface called with a null window handle");
        return false;
    }

    VkWin32SurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hwnd = static_cast<HWND>(hwnd);
    create_info.hinstance = GetModuleHandleW(nullptr);

    if (!NF_VK_CHECK(vkCreateWin32SurfaceKHR(
            m_ctx.instance, &create_info, nullptr, &m_ctx.surface))) {
        return false;
    }

    NF_LOG_DEBUG(LogCategory::RHI, "Vulkan Win32 surface created");
    return true;
#else
    (void)hwnd;
    NF_LOG_ERROR(LogCategory::RHI, "Surface creation not implemented for this platform");
    return false;
#endif
}

bool VulkanDevice::pick_physical_device() {
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(m_ctx.instance, &device_count, nullptr);
    if (device_count == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "No Vulkan-capable GPU found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(m_ctx.instance, &device_count, devices.data());

    // Prefer a discrete GPU, but fall back to anything that can present.
    for (const auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_ctx.physical_device = dev;
            m_ctx.device_properties = props;
            NF_LOG_INFO(LogCategory::RHI, "Selected GPU: {}", props.deviceName);
            break;
        }
    }

    if (m_ctx.physical_device == VK_NULL_HANDLE) {
        m_ctx.physical_device = devices[0];
        vkGetPhysicalDeviceProperties(m_ctx.physical_device, &m_ctx.device_properties);
        NF_LOG_INFO(LogCategory::RHI, "Selected GPU (fallback): {}",
                    m_ctx.device_properties.deviceName);
    }

    vkGetPhysicalDeviceMemoryProperties(m_ctx.physical_device, &m_ctx.memory_props);
    return true;
}

bool VulkanDevice::create_logical_device() {
    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_ctx.physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_ctx.physical_device, &queue_family_count, queue_families.data());

    u32 graphics_family = u32_max;
    u32 present_family = u32_max;

    for (u32 i = 0; i < queue_family_count; ++i) {
        const bool supports_graphics =
            (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

        VkBool32 present_support = VK_FALSE;
        if (m_ctx.surface != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceSurfaceSupportKHR(m_ctx.physical_device, i, m_ctx.surface,
                                                  &present_support);
        }

        if (supports_graphics) graphics_family = i;
        if (present_support)   present_family = i;

        // Prefer a single family that does both — cheaper cross-queue sync.
        if (supports_graphics && present_support) {
            graphics_family = i;
            present_family = i;
            break;
        }
    }

    if (graphics_family == u32_max) {
        NF_LOG_ERROR(LogCategory::RHI, "No queue family supporting graphics");
        return false;
    }

    // Headless devices have no surface, so there is nothing to present to.
    if (m_ctx.surface != VK_NULL_HANDLE && present_family == u32_max) {
        NF_LOG_ERROR(LogCategory::RHI, "No queue family supporting presentation");
        return false;
    }

    if (present_family == u32_max) {
        present_family = graphics_family;
    }

    m_ctx.graphics_family = graphics_family;
    m_ctx.present_family = present_family;

    std::set<u32> unique_families = {graphics_family, present_family};
    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;

    const float queue_priority = 1.0f;
    for (u32 family : unique_families) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(qci);
    }

    // Presentation is only requested when we actually have something to
    // present to; a headless device must not fail just because the swapchain
    // extension is unavailable.
    std::vector<const char*> extensions;
    if (m_ctx.surface != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = VK_TRUE; // wireframe rendering in the editor

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<u32>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.pEnabledFeatures = &features;

    if (!NF_VK_CHECK(vkCreateDevice(m_ctx.physical_device, &create_info, nullptr,
                                     &m_ctx.device))) {
        return false;
    }

    // Device-level entry points only exist once the VkDevice does, and
    // retrieving the queues below requires one of them. Load them here so the
    // ordering is enforced structurally rather than by convention.
    if (!load_device_functions(m_ctx.device)) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to load Vulkan device functions");
        // The device stays alive on purpose: destroying it would need
        // vkDestroyDevice, which is itself a device function and therefore not
        // resolvable here. shutdown() runs in the same degraded mode, so the
        // leak is bounded to a path that already cannot continue.
        return false;
    }

    vkGetDeviceQueue(m_ctx.device, graphics_family, 0, &m_ctx.graphics_queue);
    vkGetDeviceQueue(m_ctx.device, present_family, 0, &m_ctx.present_queue);

    vk_set_object_name(m_ctx.device, VK_OBJECT_TYPE_DEVICE,
                       reinterpret_cast<u64>(m_ctx.device), "NOVAForge Device");

    NF_LOG_DEBUG(LogCategory::RHI, "Vulkan logical device created (gfx queue {}, present queue {})",
                 graphics_family, present_family);
    return true;
}

bool VulkanDevice::create_command_pool() {
    VkCommandPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_info.queueFamilyIndex = m_ctx.graphics_family;

    if (!NF_VK_CHECK(vkCreateCommandPool(m_ctx.device, &create_info, nullptr,
                                          &m_ctx.command_pool))) {
        return false;
    }
    return true;
}

u32 VulkanDevice::find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) const {
    for (u32 i = 0; i < m_ctx.memory_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (m_ctx.memory_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    NF_LOG_ERROR(LogCategory::RHI, "No memory type matching filter {:#x}", type_filter);
    return u32_max;
}

// --- One-shot command helper ----------------------------------------------

bool VulkanDevice::execute_single_time_commands(
    const std::function<void(VkCommandBuffer)>& record) {

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = m_ctx.command_pool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!NF_VK_CHECK(vkAllocateCommandBuffers(m_ctx.device, &alloc_info, &cmd))) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    bool ok = NF_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));
    if (ok) {
        record(cmd);
        ok = NF_VK_CHECK(vkEndCommandBuffer(cmd));
    }

    if (ok) {
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;

        ok = NF_VK_CHECK(vkQueueSubmit(m_ctx.graphics_queue, 1, &submit_info, VK_NULL_HANDLE));

        if (ok) {
            // Waiting on the queue is the simplest correct barrier here; this
            // path is for uploads only, never for per-frame work.
            vkQueueWaitIdle(m_ctx.graphics_queue);
        }
    }

    vkFreeCommandBuffers(m_ctx.device, m_ctx.command_pool, 1, &cmd);
    return ok;
}

// --- Submission & synchronization -----------------------------------------

void VulkanDevice::submit(const CommandBuffer& cmd, const SubmitInfo& info) {
    const auto* vk_cmd = static_cast<const VulkanCommandBuffer*>(&cmd);
    if (!vk_cmd || !vk_cmd->valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "submit() called with an invalid command buffer");
        return;
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;

    wait_semaphores.reserve(info.wait_semaphores.size());
    wait_stages.reserve(info.wait_semaphores.size());

    for (usize i = 0; i < info.wait_semaphores.size(); ++i) {
        const auto* sem = static_cast<const VulkanSemaphore*>(info.wait_semaphores[i]);
        if (!sem || !sem->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "submit() received an invalid wait semaphore");
            return;
        }
        wait_semaphores.push_back(sem->handle());

        const PipelineStage stage =
            (i < info.wait_stages.size()) ? info.wait_stages[i]
                                          : PipelineStage::AllCommands;
        wait_stages.push_back(to_vk_stage(stage));
    }

    std::vector<VkSemaphore> signal_semaphores;
    signal_semaphores.reserve(info.signal_semaphores.size());
    for (const Semaphore* sem : info.signal_semaphores) {
        const auto* vk_sem = static_cast<const VulkanSemaphore*>(sem);
        if (!vk_sem || !vk_sem->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "submit() received an invalid signal semaphore");
            return;
        }
        signal_semaphores.push_back(vk_sem->handle());
    }

    VkFence fence = VK_NULL_HANDLE;
    if (info.signal_fence) {
        const auto* vk_fence = static_cast<const VulkanFence*>(info.signal_fence);
        if (!vk_fence || !vk_fence->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "submit() received an invalid fence");
            return;
        }
        fence = vk_fence->handle();
    }

    VkCommandBuffer native_cmd = vk_cmd->handle();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = static_cast<u32>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &native_cmd;
    submit_info.signalSemaphoreCount = static_cast<u32>(signal_semaphores.size());
    submit_info.pSignalSemaphores = signal_semaphores.data();

    NF_VK_CHECK(vkQueueSubmit(m_ctx.graphics_queue, 1, &submit_info, fence));
}

void VulkanDevice::wait_idle() {
    if (m_ctx.device && vkDeviceWaitIdle) {
        vkDeviceWaitIdle(m_ctx.device);
    }
}

// --- Factory methods ------------------------------------------------------

std::unique_ptr<Swapchain> VulkanDevice::create_swapchain(const SwapchainDesc& desc) {
    if (m_ctx.surface == VK_NULL_HANDLE) {
        NF_LOG_ERROR(LogCategory::RHI,
            "create_swapchain on a headless device — init() was called without a window");
        return nullptr;
    }

    auto swapchain = std::make_unique<VulkanSwapchain>(this, desc);
    if (!swapchain->valid()) return nullptr;
    return swapchain;
}

std::unique_ptr<Buffer> VulkanDevice::create_buffer(const BufferDesc& desc) {
    auto buffer = std::make_unique<VulkanBuffer>(this, desc);
    if (!buffer->valid()) return nullptr;
    return buffer;
}

std::unique_ptr<Texture> VulkanDevice::create_texture(const TextureDesc& desc) {
    auto texture = std::make_unique<VulkanTexture>(this, desc);
    if (!texture->valid()) return nullptr;
    return texture;
}

std::unique_ptr<TextureView> VulkanDevice::create_texture_view(const Texture& texture) {
    // Legacy path — delegates to the generic descriptor with defaults
    TextureViewDesc desc{};
    desc.texture = &texture;
    desc.dimension = ViewDimension::View2D;
    desc.aspect = ImageAspect::Color;
    desc.format = Format::Unknown;
    desc.base_mip = 0;
    desc.mip_count = 1;
    desc.base_layer = 0;
    desc.layer_count = 1;
    auto view = std::make_unique<VulkanTextureView>(this, desc);
    if (!view->valid()) return nullptr;
    return view;
}

std::unique_ptr<TextureView> VulkanDevice::create_texture_view(const TextureViewDesc& desc) {
    if (!desc.texture) {
        NF_LOG_ERROR(LogCategory::RHI, "TextureViewDesc::texture is null");
        return nullptr;
    }
    auto view = std::make_unique<VulkanTextureView>(this, desc);
    if (!view->valid()) return nullptr;
    return view;
}

std::unique_ptr<DescriptorAllocator> VulkanDevice::create_descriptor_allocator(u32 sets_per_pool) {
    auto alloc = std::make_unique<VulkanDescriptorAllocator>(this, sets_per_pool);
    return alloc;
}

std::unique_ptr<UploadContext> VulkanDevice::create_upload_context() {
    auto ctx = std::make_unique<VulkanUploadContext>(this);
    if (!ctx->valid()) return nullptr;
    return ctx;
}

std::unique_ptr<ShaderModule> VulkanDevice::create_shader_module(const ShaderModuleDesc& desc) {
    auto module = std::make_unique<VulkanShaderModule>(this, desc);
    if (!module->valid()) return nullptr;
    return module;
}

std::unique_ptr<Pipeline> VulkanDevice::create_pipeline(const PipelineDesc& desc) {
    auto pipeline = std::make_unique<VulkanPipeline>(this, desc);
    if (!pipeline->valid()) return nullptr;
    return pipeline;
}

std::unique_ptr<RenderPass> VulkanDevice::create_render_pass(const RenderPassDesc& desc) {
    auto pass = std::make_unique<VulkanRenderPass>(this, desc);
    if (!pass->valid()) return nullptr;
    return pass;
}

std::unique_ptr<Framebuffer> VulkanDevice::create_framebuffer(
    const RenderPass& pass,
    std::span<Texture* const> color_attachments,
    Texture* depth_attachment) {

    const auto& vk_pass = static_cast<const VulkanRenderPass&>(pass);
    auto fb = std::make_unique<VulkanFramebuffer>(this, vk_pass, color_attachments,
                                                   depth_attachment);
    if (!fb->valid()) return nullptr;
    return fb;
}

std::unique_ptr<CommandBuffer> VulkanDevice::create_command_buffer() {
    auto cmd = std::make_unique<VulkanCommandBuffer>(this);
    if (!cmd->valid()) return nullptr;
    return cmd;
}

std::unique_ptr<Semaphore> VulkanDevice::create_semaphore() {
    auto sem = std::make_unique<VulkanSemaphore>(this);
    if (!sem->valid()) return nullptr;
    return sem;
}

std::unique_ptr<Fence> VulkanDevice::create_fence(bool signaled) {
    auto fence = std::make_unique<VulkanFence>(this, signaled);
    if (!fence->valid()) return nullptr;
    return fence;
}

std::unique_ptr<Sampler> VulkanDevice::create_sampler(const SamplerDesc& desc) {
    auto sampler = std::make_unique<VulkanSampler>(this, desc);
    if (!sampler->valid()) return nullptr;
    return sampler;
}

std::unique_ptr<DescriptorSetLayout> VulkanDevice::create_descriptor_set_layout(
    const DescriptorSetLayoutDesc& desc) {
    auto layout = std::make_unique<VulkanDescriptorSetLayout>(this, desc);
    if (!layout->valid()) return nullptr;
    return layout;
}

std::unique_ptr<DescriptorSet> VulkanDevice::create_descriptor_set(
    const DescriptorSetLayout& layout) {
    const auto& vk_layout = static_cast<const VulkanDescriptorSetLayout&>(layout);
    auto set = std::make_unique<VulkanDescriptorSet>(this, vk_layout);
    if (!set->valid()) return nullptr;
    return set;
}

void VulkanDevice::update_descriptor_set(const DescriptorSet& set,
                                         std::span<const DescriptorWrite> writes) {
    VkDescriptorSet vk_handle = VK_NULL_HANDLE;
    if (auto* s = dynamic_cast<const VulkanDescriptorSet*>(&set)) {
        if (!s->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "update_descriptor_set received an invalid descriptor set");
            return;
        }
        vk_handle = s->handle();
    } else if (auto* p = dynamic_cast<const VulkanDescriptorSetPooled*>(&set)) {
        if (!p->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "update_descriptor_set received an invalid pooled descriptor set");
            return;
        }
        vk_handle = p->handle();
    } else {
        NF_LOG_ERROR(LogCategory::RHI, "update_descriptor_set received an unknown DescriptorSet implementation");
        return;
    }

    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> vk_writes;
    buffer_infos.reserve(writes.size());
    image_infos.reserve(writes.size());
    vk_writes.reserve(writes.size());

    for (const DescriptorWrite& w : writes) {
        VkWriteDescriptorSet vw{};
        vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vw.dstSet = vk_handle;
        vw.dstBinding = w.binding;
        vw.dstArrayElement = 0;
        vw.descriptorCount = 1;
        vw.descriptorType = to_vk_descriptor_type(w.type);

        if (w.type == DescriptorType::UniformBuffer && w.buffer) {
            const auto* vk_buf = static_cast<const VulkanBuffer*>(w.buffer);
            VkDescriptorBufferInfo info{};
            info.buffer = vk_buf->handle();
            info.offset = static_cast<VkDeviceSize>(w.buffer_offset);
            info.range = w.buffer_range == 0
                ? VK_WHOLE_SIZE
                : static_cast<VkDeviceSize>(w.buffer_range);
            buffer_infos.push_back(info);
            vw.pBufferInfo = &buffer_infos.back();
        }

        if ((w.type == DescriptorType::SampledImage ||
             w.type == DescriptorType::SampledImageSeparate) && w.texture_view) {
            const auto* vk_tv = static_cast<const VulkanTextureView*>(w.texture_view);
            VkDescriptorImageInfo info{};
            info.imageView = vk_tv->view();
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            info.sampler = w.sampler
                ? static_cast<const VulkanSampler*>(w.sampler)->handle()
                : VK_NULL_HANDLE;
            image_infos.push_back(info);
            vw.pImageInfo = &image_infos.back();
        }

        if (w.type == DescriptorType::Sampler && w.sampler) {
            VkDescriptorImageInfo info{};
            info.sampler = static_cast<const VulkanSampler*>(w.sampler)->handle();
            image_infos.push_back(info);
            vw.pImageInfo = &image_infos.back();
        }

        vk_writes.push_back(vw);
    }

    if (!vk_writes.empty()) {
        vkUpdateDescriptorSets(m_ctx.device,
                               static_cast<u32>(vk_writes.size()),
                               vk_writes.data(), 0, nullptr);
    }
}

} // namespace nf::rhi
