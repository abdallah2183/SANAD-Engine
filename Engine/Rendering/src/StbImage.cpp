// NF/Rendering/StbImage.cpp — single home of the stb_image implementation,
// plus the warning-clean decode facade (see ImageDecode.hpp).
//
// stb_image.h is not warning-clean by design, so the implementation stays
// quarantined here; every other translation unit talks to decode_image_*
// only and keeps the engine's /W4 /WX policy.

#include <NF/Rendering/ImageDecode.hpp>

#include <climits>
#include <cstddef>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace nf::rendering {

DecodedImage decode_image_memory(const uint8_t* data, size_t size, std::string& out_err) {
    DecodedImage out;
    if (data == nullptr || size == 0) {
        out_err = "Empty image data";
        return out;
    }
    int w = 0, h = 0, comp = 0;
    // stb_image takes int size; guard absurd inputs before the cast.
    if (size > static_cast<size_t>(INT_MAX)) {
        out_err = "Image data too large";
        return out;
    }
    unsigned char* px =
        stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, STBI_rgb_alpha);
    if (px == nullptr) {
        out_err = std::string("Decode failed: ") + stbi_failure_reason();
        return out;
    }
    out.width = w;
    out.height = h;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    out.rgba.assign(px, px + bytes);
    stbi_image_free(px);
    out_err.clear();
    return out;
}

DecodedImage decode_image_file(const std::string& physical_path, std::string& out_err) {
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(physical_path.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    if (px == nullptr) {
        out_err = std::string("Decode failed for '") + physical_path + "': " + stbi_failure_reason();
        return DecodedImage{};
    }
    DecodedImage out;
    out.width = w;
    out.height = h;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    out.rgba.assign(px, px + bytes);
    stbi_image_free(px);
    out_err.clear();
    return out;
}

} // namespace nf::rendering
