# NFRHI — Rendering Hardware Interface abstraction
#
# The Vulkan backend resolves every entry point at runtime (see
# VulkanLoader.cpp), so we only need the SDK headers — there is no import
# library to link and no hard runtime dependency on vulkan-1.dll. That is what
# lets a dedicated-server build ship with no GPU support at all.

set(NFRHI_SOURCES
    src/RHI.cpp
)

if(NF_USE_VULKAN)
    list(APPEND NFRHI_SOURCES
        src/Vulkan/Device_Vk.cpp
        src/Vulkan/VulkanLoader.cpp
        src/Vulkan/VulkanContext.cpp
        src/Vulkan/Swapchain_Vk.cpp
        src/Vulkan/CommandBuffer_Vk.cpp
        src/Vulkan/Buffer_Vk.cpp
        src/Vulkan/Texture_Vk.cpp
        src/Vulkan/Pipeline_Vk.cpp
        src/Vulkan/ShaderModule_Vk.cpp
        src/Vulkan/Descriptor_Vk.cpp
    )
endif()

nf_engine_library(NFRHI
    SOURCES ${NFRHI_SOURCES}
    DEPENDS NFCore
)

# Internal implementation headers live under src/, next to their sources.
target_include_directories(NFRHI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NF_USE_VULKAN)
    target_compile_definitions(NFRHI
        PUBLIC  NF_VULKAN_ENABLED=1
        PRIVATE VK_NO_PROTOTYPES
    )

    # --- Locate the Vulkan SDK ------------------------------------------
    # Priority: explicit cache variable, then VULKAN_SDK env var, then the
    # default Windows install location (newest version wins).
    if(NOT NF_VULKAN_SDK_PATH)
        if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
            set(NF_VULKAN_SDK_PATH "$ENV{VULKAN_SDK}")
        elseif(WIN32 AND IS_DIRECTORY "C:/VulkanSDK")
            file(GLOB _nf_vulkan_versions RELATIVE "C:/VulkanSDK" "C:/VulkanSDK/*")
            set(_nf_best_version "")
            foreach(_v IN LISTS _nf_vulkan_versions)
                if(IS_DIRECTORY "C:/VulkanSDK/${_v}" AND "${_v}" STRGREATER "${_nf_best_version}")
                    set(_nf_best_version "${_v}")
                endif()
            endforeach()
            if(NOT _nf_best_version STREQUAL "")
                set(NF_VULKAN_SDK_PATH "C:/VulkanSDK/${_nf_best_version}")
            endif()
        endif()
    endif()

    if(NOT NF_VULKAN_SDK_PATH OR NOT EXISTS "${NF_VULKAN_SDK_PATH}/Include/vulkan/vulkan.h")
        message(FATAL_ERROR
            "Vulkan SDK headers not found.\n"
            "Install the LunarG Vulkan SDK, then either set the VULKAN_SDK "
            "environment variable or pass -DNF_VULKAN_SDK_PATH=<sdk dir>.\n"
            "(Searched: '${NF_VULKAN_SDK_PATH}')")
    endif()

    set(NF_VULKAN_SDK_PATH "${NF_VULKAN_SDK_PATH}" CACHE PATH "Path to the Vulkan SDK")
    message(STATUS "Vulkan SDK: ${NF_VULKAN_SDK_PATH}")

    target_include_directories(NFRHI PRIVATE "${NF_VULKAN_SDK_PATH}/Include")
endif()
