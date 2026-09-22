#pragma once

// NF/Editor/PreviewPixels.hpp — CPU-side downsample for editor thumbnails.
//
// Asset previews are a UI concern: the runtime uploads full-resolution textures
// for the scene, but an inspector swatch or a browser thumbnail only ever needs
// ~128 pixels on a side. Re-uploading the source at full size would burn GPU
// memory for something the user never sees at that resolution, so the preview
// path decodes, shrinks here, and uploads only the small result.
//
// This module is deliberately free of RHI and of the image decoder: it takes
// raw RGBA8 bytes and gives back raw RGBA8 bytes, so it stays in NFEditorCore
// where EditorTests can cover it. The shell wires the decoder and the GPU.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <vector>

namespace nf::editor {

struct PreviewPixels {
    int width = 0;
    int height = 0;
    std::vector<u8> rgba; // width * height * 4, row-major, top-left origin

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }
};

/// Box-averages an RGBA8 image so its longest side is at most `max_dim`,
/// preserving aspect. A source already at or under `max_dim` is returned as a
/// copy at its original size rather than magnified — a thumbnail is allowed to
/// be small, but averaging up would blur an image the GPU can show sharp.
///
/// Returns an invalid result for null pixels, non-positive dimensions, or a
/// non-positive `max_dim`; the caller draws a placeholder rather than a
/// half-initialised texture.
PreviewPixels downsample_rgba(const u8* rgba, int width, int height, int max_dim);

} // namespace nf::editor
