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

/// Translated string for `key` (logical UTF-8). Missing/empty values fall
/// back: Arabic value -> English value -> the key itself (never empty; a
/// miss is logged once). Lookup is an unordered_map built once from the
/// table — NOT a linear scan per call (tr runs per widget per frame).
std::string tr(const char* key);

/// Number of UNIQUE keys registered (for tests/tools).
unsigned tr_key_count();

/// Raw source-table row count and its keys. Tests walk these to prove the
/// table carries no duplicate key (a duplicate would silently shadow the
/// later row).
unsigned tr_table_count();
const char* tr_table_key_at(unsigned index);
unsigned tr_duplicate_count();

} // namespace nf::ui
