#pragma once

// NF/UI/Localization.hpp — editor UI string tables (Phase 15).
//
// tr("key") returns the English source string by default and the Arabic
// translation after set_language(Language::Arabic). Unknown keys return the
// key itself (fail visible, never empty). Arabic strings are logical-order
// UTF-8; render them through ui::shape_arabic() (see ArabicShaper.hpp).

#include <string>

namespace nf::ui {

enum class Language : unsigned char {
    English = 0,
    Arabic = 1,
};

void set_language(Language lang);
Language current_language();

/// Translated string for `key` (logical UTF-8). Falls back to English, then
/// to the key itself.
std::string tr(const char* key);

/// Number of registered keys (for tests/tools).
unsigned tr_key_count();

} // namespace nf::ui
