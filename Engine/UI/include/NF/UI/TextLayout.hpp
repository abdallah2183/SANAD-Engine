#pragma once
// NF/UI/TextLayout.hpp — measured text layout shared by ScriptCanvas and GameUiOverlay.
//
// Wrapping and tooltip sizing must use the SAME measurement the draw backend
// uses (ImFont::CalcTextSizeA on the shaped/visual string). The old tooltip
// sized itself from a fixed estimate, so its box and the drawn text disagreed.
// This module is the single layout implementation: it shapes each visual line
// exactly once, measures through an injected callback, and clips an over-wide
// word by codepoint (never mid UTF-8 sequence, never mid ligature).
//
// Scope: Arabic/Urdu/Farsi shaping and simplified bidi come from
// ArabicShaper.hpp. Hebrew is not shaped; full Unicode BiDi and HarfBuzz
// fine positioning are NOT provided. See that header.
//
// All logic here is pure CPU: tests inject a deterministic measurer and need
// no GPU, no ImGui and no window.

#include <NF/UI/ArabicShaper.hpp>

#include <functional>
#include <string>
#include <vector>

namespace nf::ui {

/// Measures the width (pixels) of a VISUAL (already shaped) string at
/// font_size. GameUiOverlay supplies ImFont measurement; tests supply a fake.
using TextMeasurer = std::function<float(const std::string& shaped_text, float font_size)>;

/// Fallback measurer: monospace-ish estimate. Only used headless (no font).
/// Not a char-count tooltip: wrap + tooltip sizes still derive from this
/// measurement, so a wide/narrow measurer changes the layout.
float fallback_text_width(const std::string& shaped_text, float font_size);

/// --- UTF-8 helpers (codepoint aware, never split a sequence) --------------
/// Byte offset of the codepoint boundary after byte `offset`'s codepoint.
/// Returns offset if invalid.
size_t utf8_next_boundary(std::string_view utf8, size_t offset);
/// Byte offset of the boundary BEFORE the codepoint ending at `offset`.
size_t utf8_prev_boundary(std::string_view utf8, size_t offset);
/// Number of codepoints (invalid bytes each count as one).
size_t utf8_codepoint_count(std::string_view utf8);
/// Clamp a byte offset DOWN to the nearest codepoint boundary.
size_t utf8_snap_boundary(std::string_view utf8, size_t offset);
/// Decode one codepoint at byte offset; returns encoded length or 0 if invalid.
size_t utf8_decode_one(std::string_view utf8, size_t offset, char32_t& out_cp);
/// Encode one codepoint to UTF-8. Rejects surrogates/out-of-range (returns 0).
size_t utf8_encode(char32_t cp, std::string& out);
/// True when the string is valid UTF-8 (no overlong/surrogate truncation).
bool utf8_valid(std::string_view utf8);

/// --- Wrapping --------------------------------------------------------------
/// Wraps logical UTF-8 into visual lines fitting box_w.
/// - Hard lines split on '\n'; empty lines survive.
/// - Words are space separated; a word wider than the box is clipped to the
///   longest codepoint prefix that fits, suffixed with '…' (U+2026) — the box
///   edge is never exceeded (overflow lines are clipped, not split mid-word).
/// - Each visual line is shaped exactly once.
/// - Returned lines are visual (presentation forms / RTL run order).
std::vector<std::string> wrap_text(std::string_view logical, float box_w, float font_size,
                                   const TextMeasurer& measure);

/// Width of the longest wrapped line (for auto-size boxes).
float wrapped_max_width(const std::vector<std::string>& visual_lines, float font_size,
                        const TextMeasurer& measure);

/// Line height for a font size (matches the draw backend).
float line_height_for(float font_size);

struct TooltipLayout {
    float x = 0, y = 0;   // top-left in pixels
    float w = 0, h = 0;   // box size in pixels
    std::vector<std::string> lines; // visual lines, inner width fits
};

/// Computes a tooltip box from font measurement: max width capped at 360px
/// (and 50% of the viewport), inner padding 8px per side + 6px top/bottom,
/// height from the wrapped line count. The box is clamped so it stays inside
/// the viewport with a 4px screen inset; tall tooltips are height-clamped.
TooltipLayout layout_tooltip(std::string_view logical, float viewport_w, float viewport_h,
                             float pointer_x_px, float pointer_y_px, float font_size,
                             const TextMeasurer& measure);

/// Caps a single visual word to box_w by clipping at a codepoint boundary and
/// appending '…'. If even one codepoint + ellipsis exceeds the box, the word
/// is ellipsized to just '…' (never empty, never over-wide where possible).
std::string clip_word_to_width(std::string_view logical_word, float box_w, float font_size,
                               const TextMeasurer& measure);

} // namespace nf::ui
