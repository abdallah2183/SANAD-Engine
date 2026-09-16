#pragma once

// NF/UI/ArabicShaper.hpp — Arabic shaping + bidi for UI text (Phase 15).
//
// Stock TrueType rasterizers (stb_truetype, which ImGui uses) draw glyphs
// left-to-right with no Arabic joining or reordering, so logical Arabic
// ("السلام") would render disconnected and backwards. This module converts
// logical UTF-8 into visual UTF-8: contextual presentation forms (data from
// the Unicode Character Database, verified at vendor time), lam-alef
// ligatures, and directional-run reordering.
//
// Scope (documented, not accidental):
//   - Arabic block + Farsi/Urdu extras (see the table in ArabicShaper.cpp).
//   - Harakat (tashkeel) pass through attached to their base letter; they
//     are transparent for joining. Fine positioning of marks needs a real
//     text stack (HarfBuzz) — a later phase.
//   - Bidi is a simplified Unicode Bidirectional Algorithm: RTL runs (Arabic
//     letters/marks/numbers) reverse; Latin runs and digit runs keep order;
//     neutrals join the surrounding strong direction. Good for UI labels,
//     not for a text editor.
//   - Hebrew is not shaped (no Hebrew presentation need in this engine today);
//     Hebrew letters are treated as RTL neutrals for ordering.

#include <string>

namespace nf::ui {

/// True for Arabic-script letters that join (U+0621–U+064A + extras).
bool is_arabic_letter(char32_t cp);
/// True for Arabic diacritics (harakat U+064B–U+0652, U+0670, ...).
bool is_arabic_mark(char32_t cp);
/// True for Arabic-Indic digits (U+0660–U+0669, U+06F0–U+06F9) and ASCII digits.
bool is_digit(char32_t cp);
/// True for characters that flow right-to-left (Arabic letters/marks/digits).
bool is_rtl_char(char32_t cp);

/// Logical UTF-8 -> visual UTF-8. Pure ASCII/Latin input passes through
/// byte-identical (fast path, no allocation surprises).
std::string shape_arabic(const std::string& logical_utf8);

/// True when shaping would change anything (lets callers skip work).
bool needs_shaping(const std::string& utf8);

} // namespace nf::ui
