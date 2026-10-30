#pragma once

#include <NF/Rendering/Terrain.hpp>
#include <NF/Core/Types.hpp>
#include <vector>

namespace nf::rendering {

/// Simulation parameters for hydraulic droplet erosion and thermal talus relaxation.
struct TerrainErosionSettings {
    // Hydraulic droplet simulation
    u32 dropletCount = 5000;            // Number of rain droplets to simulate
    u32 maxDropletLifetime = 40;        // Maximum steps per droplet
    float inertia = 0.05f;              // Droplet momentum direction persistence [0, 1]
    float sedimentCapacityFactor = 4.0f;// Multiplier for sediment carrying capacity
    float minSedimentCapacity = 0.01f;  // Base minimal sediment carried by droplet
    float erodeSpeed = 0.3f;            // Rate at which terrain dissolves into sediment [0, 1]
    float depositSpeed = 0.3f;          // Rate at which excess sediment settles into terrain [0, 1]
    float evaporateSpeed = 0.01f;       // Rate at which droplet water evaporates per step [0, 1]
    float gravity = 4.0f;               // Gravity acceleration pulling droplet downhill
    float initialWater = 1.0f;          // Starting water volume of each droplet
    float initialSpeed = 1.0f;          // Starting speed of each droplet

    // Thermal (talus angle / repose angle) relaxation
    u32 thermalIterations = 5;          // Number of thermal diffusion passes
    float talusAngleThreshold = 0.04f;  // Maximum height difference between neighbors before slippage
    float thermalSlippageRate = 0.5f;   // Fraction of excess material transferred downhill [0, 1]

    u32 randomSeed = 1337;
};

/// Terrain erosion simulator for procedural heightfields.
class TerrainErosion {
public:
    /// Runs hydraulic droplet-based erosion on the given heightfield in-place.
    static void simulate_hydraulic(HeightField& field, const TerrainErosionSettings& settings);

    /// Runs thermal (talus angle relaxation) erosion on the given heightfield in-place.
    static void simulate_thermal(HeightField& field, const TerrainErosionSettings& settings);

    /// Runs full erosion pipeline (hydraulic simulation followed by thermal relaxation).
    static void simulate(HeightField& field, const TerrainErosionSettings& settings);

    /// Generates a difference (erosion/deposition) map between original and eroded fields.
    /// Positive values indicate deposition (sediment gained); negative values indicate erosion (sediment carved).
    [[nodiscard]] static std::vector<float> compute_sediment_map(
        const HeightField& original,
        const HeightField& eroded
    );

    /// Computes analytical height gradient (dh/dx, dh/dy) at sub-pixel coordinate (posX, posY).
    static void compute_gradient(
        const HeightField& field,
        float posX, float posY,
        float& outGradX, float& outGradY,
        float& outHeight
    );
};

} // namespace nf::rendering
