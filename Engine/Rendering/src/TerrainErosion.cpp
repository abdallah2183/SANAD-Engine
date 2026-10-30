#include <NF/Rendering/TerrainErosion.hpp>
#include <algorithm>
#include <cmath>

namespace nf::rendering {

void TerrainErosion::compute_gradient(
    const HeightField& field,
    float posX, float posY,
    float& outGradX, float& outGradY,
    float& outHeight
) {
    if (!field.ok()) {
        outGradX = outGradY = outHeight = 0.0f;
        return;
    }

    const u32 w = field.width;
    const u32 h = field.height;

    float clampedX = std::clamp(posX, 0.0f, static_cast<float>(w - 1));
    float clampedY = std::clamp(posY, 0.0f, static_cast<float>(h - 1));

    u32 x0 = static_cast<u32>(clampedX);
    u32 y0 = static_cast<u32>(clampedY);
    u32 x1 = std::min(x0 + 1, w - 1);
    u32 y1 = std::min(y0 + 1, h - 1);

    float u = clampedX - static_cast<float>(x0);
    float v = clampedY - static_cast<float>(y0);

    float h00 = field.heights[y0 * w + x0];
    float h10 = field.heights[y0 * w + x1];
    float h01 = field.heights[y1 * w + x0];
    float h11 = field.heights[y1 * w + x1];

    outGradX = (h10 - h00) * (1.0f - v) + (h11 - h01) * v;
    outGradY = (h01 - h00) * (1.0f - u) + (h11 - h10) * u;

    outHeight = h00 * (1.0f - u) * (1.0f - v) +
                h10 * u * (1.0f - v) +
                h01 * (1.0f - u) * v +
                h11 * u * v;
}

void TerrainErosion::simulate_hydraulic(HeightField& field, const TerrainErosionSettings& settings) {
    if (!field.ok() || field.width < 2 || field.height < 2 || settings.dropletCount == 0) return;

    const u32 w = field.width;
    const u32 h = field.height;

    u32 state = settings.randomSeed == 0 ? 1337u : settings.randomSeed;
    auto lcg_float = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state) / 4294967296.0f;
    };

    auto deposit = [&](float px, float py, float amount) {
        u32 x0 = static_cast<u32>(px);
        u32 y0 = static_cast<u32>(py);
        u32 x1 = std::min(x0 + 1, w - 1);
        u32 y1 = std::min(y0 + 1, h - 1);

        float u = px - static_cast<float>(x0);
        float v = py - static_cast<float>(y0);

        field.heights[y0 * w + x0] += amount * (1.0f - u) * (1.0f - v);
        field.heights[y0 * w + x1] += amount * u * (1.0f - v);
        field.heights[y1 * w + x0] += amount * (1.0f - u) * v;
        field.heights[y1 * w + x1] += amount * u * v;
    };

    auto erode = [&](float px, float py, float amount) {
        deposit(px, py, -amount);
    };

    for (u32 drop = 0; drop < settings.dropletCount; ++drop) {
        float posX = lcg_float() * static_cast<float>(w - 2) + 0.5f;
        float posY = lcg_float() * static_cast<float>(h - 2) + 0.5f;

        float dirX = 0.0f;
        float dirY = 0.0f;
        float speed = settings.initialSpeed;
        float water = settings.initialWater;
        float sediment = 0.0f;

        for (u32 step = 0; step < settings.maxDropletLifetime; ++step) {
            float gradX = 0.0f, gradY = 0.0f, currHeight = 0.0f;
            compute_gradient(field, posX, posY, gradX, gradY, currHeight);

            // Update direction with inertia and gravity pulling along negative gradient
            dirX = dirX * settings.inertia - gradX * (1.0f - settings.inertia);
            dirY = dirY * settings.inertia - gradY * (1.0f - settings.inertia);

            float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
            if (dirLen > 1e-6f) {
                dirX /= dirLen;
                dirY /= dirLen;
            } else {
                dirX = lcg_float() * 2.0f - 1.0f;
                dirY = lcg_float() * 2.0f - 1.0f;
            }

            float newPosX = posX + dirX;
            float newPosY = posY + dirY;

            // Stop if droplet flows off the map boundaries
            if (newPosX < 0.5f || newPosX >= static_cast<float>(w - 1.5f) ||
                newPosY < 0.5f || newPosY >= static_cast<float>(h - 1.5f)) {
                break;
            }

            float nextGradX = 0.0f, nextGradY = 0.0f, nextHeight = 0.0f;
            compute_gradient(field, newPosX, newPosY, nextGradX, nextGradY, nextHeight);

            float heightDelta = nextHeight - currHeight;

            // Calculate sediment capacity
            float slopeFactor = std::max(-heightDelta, settings.minSedimentCapacity);
            float capacity = slopeFactor * speed * water * settings.sedimentCapacityFactor;

            if (sediment > capacity || heightDelta > 0.0f) {
                // Deposit excess sediment
                float amountToDeposit = (heightDelta > 0.0f)
                    ? std::min(sediment, heightDelta)
                    : (sediment - capacity) * settings.depositSpeed;
                amountToDeposit = std::clamp(amountToDeposit, 0.0f, sediment);

                sediment -= amountToDeposit;
                deposit(posX, posY, amountToDeposit);
            } else {
                // Erode terrain
                float amountToErode = std::min((capacity - sediment) * settings.erodeSpeed, -heightDelta);
                amountToErode = std::max(0.0f, amountToErode);

                sediment += amountToErode;
                erode(posX, posY, amountToErode);
            }

            // Update physical properties
            speed = std::sqrt(std::max(0.01f, speed * speed + heightDelta * settings.gravity));
            water *= (1.0f - settings.evaporateSpeed);

            if (water < 1e-3f) {
                // Evaporate remaining sediment onto ground
                deposit(posX, posY, sediment);
                break;
            }

            posX = newPosX;
            posY = newPosY;
        }
    }
}

void TerrainErosion::simulate_thermal(HeightField& field, const TerrainErosionSettings& settings) {
    if (!field.ok() || field.width < 2 || field.height < 2 || settings.thermalIterations == 0) return;

    const u32 w = field.width;
    const u32 h = field.height;

    std::vector<float> buffer = field.heights;

    const int dx[4] = { 1, -1, 0, 0 };
    const int dy[4] = { 0, 0, 1, -1 };

    for (u32 iter = 0; iter < settings.thermalIterations; ++iter) {
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u32 idx = y * w + x;
                float heightCenter = field.heights[idx];

                for (int dir = 0; dir < 4; ++dir) {
                    int nx = static_cast<int>(x) + dx[dir];
                    int ny = static_cast<int>(y) + dy[dir];

                    if (nx >= 0 && nx < static_cast<int>(w) && ny >= 0 && ny < static_cast<int>(h)) {
                        u32 nidx = static_cast<u32>(ny) * w + static_cast<u32>(nx);
                        float heightNeighbor = field.heights[nidx];

                        float diff = heightCenter - heightNeighbor;
                        if (diff > settings.talusAngleThreshold) {
                            float excess = (diff - settings.talusAngleThreshold) * 0.25f * settings.thermalSlippageRate;
                            buffer[idx] -= excess;
                            buffer[nidx] += excess;
                        }
                    }
                }
            }
        }
        field.heights = buffer;
    }
}

void TerrainErosion::simulate(HeightField& field, const TerrainErosionSettings& settings) {
    simulate_hydraulic(field, settings);
    simulate_thermal(field, settings);
}

std::vector<float> TerrainErosion::compute_sediment_map(
    const HeightField& original,
    const HeightField& eroded
) {
    if (!original.ok() || !eroded.ok() ||
        original.width != eroded.width ||
        original.height != eroded.height) {
        return {};
    }

    const size_t total = original.heights.size();
    std::vector<float> diff(total);
    for (size_t i = 0; i < total; ++i) {
        diff[i] = eroded.heights[i] - original.heights[i];
    }
    return diff;
}

} // namespace nf::rendering
