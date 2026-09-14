#pragma once

// NF/Rendering/ShaderReflection.hpp — SPIR-V reflection (minimal, production-safe)
//
// Extracts the metadata that Material needs from SPIR-V without relying on
// hand-written bindings in two places:
//
//   Binding, Type, Set, Binding index, Array count, Stage, Push constants
//
// The result is used as:
//
//   Shader Reflection → Material Layout → Descriptor Layout
//
// This is a minimal parser that handles the shaders used in NOVAForge today
// (sampled images, samplers, uniform buffers, push constants, vertex inputs).
// It is not a full SPIR-V parser, but it is correct for the instructions we
// emit and it fails gracefully (empty result) on unknown constructs rather than
// crashing.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <span>
#include <string>
#include <vector>

namespace nf::rendering {

struct ReflectedBinding {
    u32 set = 0;
    u32 binding = 0;
    rhi::DescriptorType type = rhi::DescriptorType::UniformBuffer;
    rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
    u32 count = 1;
    std::string name;
};

struct ReflectedPushConstant {
    u32 size = 0; // bytes
    u32 offset = 0;
    rhi::ShaderStage stages = rhi::ShaderStage::Vertex;
    std::string name;
};

struct ReflectedVertexInput {
    u32 location = 0;
    rhi::Format format = rhi::Format::Unknown;
    std::string name;
};

struct ReflectionData {
    std::vector<ReflectedBinding> bindings;
    std::vector<ReflectedPushConstant> push_constants;
    std::vector<ReflectedVertexInput> vertex_inputs;
    bool valid = false;
};

// Reflects the given SPIR-V bytecode. `stage` is the stage the module was
// compiled for (used to tag the reflected bindings). Returns an empty
// ReflectionData with valid=false if the SPIR-V is malformed.
ReflectionData reflect_spirv(std::span<const u8> spirv, rhi::ShaderStage stage);

// Helper: builds a DescriptorSetLayoutDesc directly from reflection
std::vector<rhi::DescriptorBinding> reflection_to_descriptor_bindings(const ReflectionData& refl);

} // namespace nf::rendering
