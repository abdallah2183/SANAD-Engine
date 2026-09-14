#pragma once

// NF/Rendering/ImageDecode.hpp — image file decoding (PNG/JPG/BMP/TGA/...).
//
// Thin warning-clean facade over stb_image (vendored in ThirdParty/stb; the
// implementation lives quarantined in StbImage.cpp). Always decodes to opaque
// RGBA8. Used by the runtime texture table and the asset import queue.

#include <NF/Core/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nf::rendering {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width*height*4, row-major, top-left origin
    bool ok() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

DecodedImage decode_image_file(const std::string& physical_path, std::string& out_err);
DecodedImage decode_image_memory(const uint8_t* data, size_t size, std::string& out_err);

} // namespace nf::rendering
