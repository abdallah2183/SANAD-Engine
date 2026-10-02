// UITests — editor string localisation (Phase 15).

#include <NF/Test/TestFramework.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::ui;

NF_TEST(localization_english_is_default) {
    set_language(Language::English);
    NF_CHECK(current_language() == Language::English);
    NF_CHECK(tr("sky") == "Sky");
    NF_CHECK(tr("directional_light") == "Directional Light");
    // A3: this used to be one duplicated key, and because the table lookup is a
    // first-match-wins scan the FIRST entry shadowed the second — so the Create
    // menu's "Add Sky" rendered as the inspector button's "Add Sky Settings"
    // (and the golden-hour item as "Add Sky Settings - Golden hour"). They are
    // now two distinct keys, and both must resolve to their own string.
    NF_CHECK(tr("add_sky") == "Add Sky");
    NF_CHECK(tr("add_sky_settings") == "Add Sky Settings");
}

NF_TEST(localization_arabic_translates) {
    set_language(Language::Arabic);
    NF_CHECK(current_language() == Language::Arabic);
    NF_CHECK(tr("sky") == "السماء");
    NF_CHECK(tr("directional_light") == "الإضاءة الاتجاهية");
    NF_CHECK(tr("apply") == "تطبيق");
    set_language(Language::English); // restore for other tests
    NF_CHECK(tr("sky") == "Sky");
}

NF_TEST(localization_unknown_key_returns_key) {
    set_language(Language::English);
    NF_CHECK(tr("no_such_key_xyz") == "no_such_key_xyz");
    set_language(Language::Arabic);
    NF_CHECK(tr("no_such_key_xyz") == "no_such_key_xyz");
    set_language(Language::English);
}

NF_TEST(localization_table_is_populated) {
    // A real translation table, not a stub: dozens of keys.
    NF_CHECK(tr_key_count() >= 40);
}

// --- Arabic-shell hardening: uniqueness, fallback, full coverage ------------

NF_TEST(localization_keys_are_unique) {
    // A repeated key used to shadow silently (first-match-wins): the table
    // must carry no duplicates, and the unique count must equal the raw rows.
    NF_CHECK(tr_duplicate_count() == 0u);
    NF_CHECK(tr_table_count() == tr_key_count());
    NF_CHECK(tr_table_count() > 0u);
}

namespace {

// Keys whose Arabic value is deliberately identical to English:
//   * a language name is shown in its own script in both languages;
//   * deg_fmt is a pure printf format ("%.0f°") with no words in it at all — the
//     degree sign is a symbol, so there is nothing to translate and it must stay
//     byte-identical or the number stops parsing the same way in both UIs.
bool same_in_both_languages(const std::string& key) {
    return key == "arabic_name" || key == "sh_radio_en" || key == "sh_radio_ar" ||
           key == "deg_fmt" ||
           // sh_brand is the product name. It has to be in this list, not in
           // latin_allowed_in_arabic, because the failure it prevents is a
           // hardcoded Latin literal in the launcher's sidebar (which is what
           // shipped: the brand read "سند" while the window title, the .nfproj
           // filter and the window class all said NOVAForge). Routing the brand
           // through a key is what makes ONE string the product name; the value
           // is identical in both languages because a product name is not
           // translated.
           key == "sh_brand";
}

// Keys whose Arabic value legitimately keeps Latin letters (physical key
// names, file tokens quoted verbatim, own-script language tags).
bool latin_allowed_in_arabic(const std::string& key) {
    static const char* const kAllowed[] = {
        "navigate_hint",     // WASD/QE are printed on the keyboard
        "shift_click_multi", // shift-click names a modifier chord
        "no_clips",          // quotes the `Animation:` scene-file token
        "viewport_hint_drag",
        "viewport_hint_dragging", // W/E/R + Esc are physical keys
        "press_to_start",         // Enter is a physical key
        "status_unsaved",         // Ctrl+S is a physical key chord
        "sh_help_notes_sub",      // ROADMAP.md is a filename quoted verbatim
        "sh_st_roadmap_missing",  // same filename, status-line wording
        "sh_st_nfproj_missing",   // .nfproj is the extension quoted verbatim
        "sh_st_drop_only",        // same extension, drop-target hint
        "sh_radio_ar",            // own-script tag "العربية (Arabic)"
        "rhi_objects",            // RHI is the renderer API's own name
        "frame_selection_hint",   // Home is a physical key
        "frame_all_hint",         // Shift+Home is a physical key chord
        "err_font_missing_ar",    // Resources/fonts + NOVAForgeEditor.exe: the
                                  // paths a user has to type, quoted verbatim
    };
    for (const char* k : kAllowed) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

// Strips printf verbs (%s, %d, %zu, %lld, %.2f, %%.2fs' trailing-s aside — the
// 's' of "%.2fs" is a seconds UNIT, so Arabic values use "ث" and any Latin
// left after this scan is a real word) so the Latin check below only sees
// actual text.
std::string without_format_verbs(const std::string& s) {
    static const char* const kConversions = "diuoxXfFeEgGaAcspn%";
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '%') {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t j = i + 1;
        // flags / width / precision / length, then the conversion letter.
        while (j < s.size() && (s[j] == '-' || s[j] == '+' || s[j] == ' ' || s[j] == '#' ||
                                s[j] == '0' || s[j] == '.' || s[j] == '*' || s[j] == 'l' ||
                                s[j] == 'h' || s[j] == 'j' || s[j] == 'z' || s[j] == 't' ||
                                s[j] == 'L' || (s[j] >= '0' && s[j] <= '9'))) {
            ++j;
        }
        if (j < s.size() &&
            std::string(kConversions).find(s[j]) != std::string::npos) {
            ++j; // consume the conversion: the whole verb disappears
        } else {
            out.push_back(s[i++]); // lone '%': keep it, move on
            continue;
        }
        i = j;
    }
    return out;
}

bool contains_latin_letter(const std::string& s) {
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            return true;
        }
    }
    return false;
}

} // namespace

NF_TEST(localization_every_key_has_both_languages) {
    // Fallback chain proof: no key may resolve empty in either language, and
    // (outside the own-script exemptions) Arabic must differ from English —
    // an identical value is the untranslated-UI defect.
    std::string problems;
    for (unsigned i = 0; i < tr_table_count(); ++i) {
        const char* key = tr_table_key_at(i);
        if (key == nullptr || *key == '\0') {
            problems += "\n  row has no key";
            continue;
        }
        set_language(Language::English);
        const std::string en = tr(key);
        set_language(Language::Arabic);
        const std::string ar = tr(key);
        if (en.empty() || ar.empty()) {
            problems += std::string("\n  '") + key + "': empty translation";
            continue;
        }
        if (!same_in_both_languages(key) && en == ar) {
            problems += std::string("\n  '") + key + "': Arabic falls back to English";
        }
    }
    set_language(Language::English); // restore for other tests
    NF_CHECK(problems.empty());
}

NF_TEST(localization_arabic_values_carry_real_arabic) {
    // A half-translated value renders Latin inside the Arabic UI. Format verbs
    // and the documented physical-key/file-token keys are exempt; everything
    // else must be verb-free of Latin letters and must need shaping.
    std::string problems;
    for (unsigned i = 0; i < tr_table_count(); ++i) {
        const char* key = tr_table_key_at(i);
        if (key == nullptr) {
            continue;
        }
        const std::string k = key;
        set_language(Language::Arabic);
        const std::string ar = tr(key);
        if (same_in_both_languages(k) || latin_allowed_in_arabic(k)) {
            continue;
        }
        if (!needs_shaping(ar)) {
            problems += "\n  '" + k + "': value contains no Arabic at all";
            continue;
        }
        if (contains_latin_letter(without_format_verbs(ar))) {
            problems += "\n  '" + k + "': Arabic value contains Latin text";
        }
    }
    set_language(Language::English); // restore for other tests
    NF_CHECK(problems.empty());
}

NF_TEST(localization_launcher_keys_resolve) {
    // Spot-check the shell block the launcher paints: nav, buttons, search
    // hint, status messages, name-error reasons, ago buckets.
    static const char* const kKeys[] = {
        "sh_nav_projects", "sh_nav_new",     "sh_nav_learn",  "sh_nav_store",
        "sh_nav_settings", "sh_nav_help",    "sh_btn_open_file", "sh_btn_new_project",
        "sh_btn_open_project", "sh_btn_show_folder", "sh_btn_create", "sh_btn_cancel",
        "sh_btn_browse",   "sh_btn_clear_recent", "sh_btn_docs", "sh_btn_bug",
        "sh_search_placeholder", "sh_all_projects", "sh_no_match", "sh_soon",
        "sh_st_missing", "sh_st_choose_loc", "sh_st_cleared", "err_name_empty",
        "err_name_reserved", "sh_ago_now", "sh_ago_hours", "sh_learn_title",
        "sh_help_title", "sh_settings_title", "sh_store_title", "sh_radio_ar",
        "sh_empty_title", "sh_empty_hint", "sh_tpl_blank_desc", "sh_confirm_clear_text",
        "sh_st_drop_only", "sh_st_removed", "sh_name_hint",
        // The sidebar header. `sh_brand` is the one key here whose Arabic value
        // is deliberately Latin (see same_in_both_languages): a product name is
        // not translated, and routing it through a key is what stops the sidebar
        // from carrying a second, hand-typed brand.
        "sh_brand", "sh_tagline", "sh_version", "sh_windows_only",
    };
    std::string problems;
    for (const char* key : kKeys) {
        set_language(Language::English);
        const std::string en = tr(key);
        set_language(Language::Arabic);
        const std::string ar = tr(key);
        if (en.empty() || ar.empty() || en == key || ar == key) {
            problems += std::string("\n  '") + key + "': launcher key missing";
        }
    }
    // `sh_version` carries a %s for the engine version. It used to be the word
    // "Sanad" in both languages, which is a person's name, not a version label —
    // and snprintf of a label with no verb reads as a stray word on screen. The
    // verb is what makes the number read as a version.
    set_language(Language::English);
    NF_CHECK(std::string(tr("sh_version")).find("%s") != std::string::npos);
    NF_CHECK(std::string(tr("sh_hero_engine")).find("%s") != std::string::npos);
    set_language(Language::Arabic);
    NF_CHECK(std::string(tr("sh_version")).find("%s") != std::string::npos);
    set_language(Language::English); // restore for other tests
    NF_CHECK(problems.empty());
}

NF_TEST(no_translated_string_is_an_icon_codepoint) {
    // An icon is GEOMETRY, never a codepoint. The launcher painted U+2699 (gear)
    // in an Arabic naskh face and six emoji (U+1F4D6, U+1F393, U+1F4AC, U+1F4CB,
    // U+2709, U+2605) in Segoe UI: neither face contains them, Windows
    // font-links to a COLOUR font whose colour does not survive DrawTextW, and
    // the result was a broken box in the product's own header. It is now
    // draw_vec_icon() geometry.
    //
    // This is checkable without a window, which is the only reason the class of
    // bug is worth a test: a missing glyph is invisible to every other test in
    // the suite and obvious to the user.
    const auto is_symbol_or_emoji = [](uint32_t cp) {
        // U+1F000..U+1FAFF  pictographs, symbols, emoji
        if (cp >= 0x1F000u && cp <= 0x1FAFFu) {
            return true;
        }
        // U+2600..U+27BF    miscellaneous symbols + dingbats
        if (cp >= 0x2600u && cp <= 0x27BFu) {
            return true;
        }
        // U+2B00..U+2BFF    supplementary arrows, geometric shapes
        if (cp >= 0x2B00u && cp <= 0x2BFFu) {
            return true;
        }
        // U+25A0..U+25FF    geometric shapes
        if (cp >= 0x25A0u && cp <= 0x25FFu) {
            return true;
        }
        return false;
    };

    std::string problems;
    for (unsigned i = 0; i < tr_table_count(); ++i) {
        const char* key = tr_table_key_at(i);
        if (key == nullptr) {
            continue;
        }
        for (int lang = 0; lang < 2; ++lang) {
            set_language(lang == 0 ? Language::English : Language::Arabic);
            const std::string s = tr(key);
            // Decode UTF-8 to codepoints; a partial tail byte is not a
            // codepoint and is not an icon.
            for (size_t p = 0; p < s.size();) {
                const unsigned char c0 = static_cast<unsigned char>(s[p]);
                uint32_t cp = 0xFFFDu;
                size_t len = 1;
                if (c0 < 0x80u) {
                    cp = c0;
                } else if ((c0 & 0xE0u) == 0xC0u) {
                    cp = c0 & 0x1Fu;
                    len = 2;
                } else if ((c0 & 0xF0u) == 0xE0u) {
                    cp = c0 & 0x0Fu;
                    len = 3;
                } else if ((c0 & 0xF8u) == 0xF0u) {
                    cp = c0 & 0x07u;
                    len = 4;
                }
                if (p + len > s.size()) {
                    break;
                }
                for (size_t k = 1; k < len; ++k) {
                    cp = (cp << 6) | (static_cast<unsigned char>(s[p + k]) & 0x3Fu);
                }
                if (is_symbol_or_emoji(cp)) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "U+%04X", cp);
                    problems += std::string("\n  '") + key + "' (" +
                                (lang == 0 ? "en" : "ar") + "): icon codepoint " + buf;
                }
                p += len;
            }
        }
    }
    set_language(Language::English); // restore for other tests
    NF_CHECK(problems.empty());
}

NF_TEST(localization_shaper_shapes_shell_arabic) {
    // The GDI shell passes every Arabic string through shape_arabic(): a real
    // launcher sentence must come back as presentation forms (== changed
    // bytes), while pure Latin passes byte-identical (paths stay intact).
    set_language(Language::Arabic);
    const std::string logical = tr("sh_projects_sub");
    const std::string visual = shape_arabic(logical);
    NF_CHECK(!logical.empty());
    NF_CHECK(visual != logical);
    NF_CHECK(needs_shaping(logical));
    NF_CHECK(shape_arabic("Search projects") == "Search projects");
    NF_CHECK(shape_arabic("Sanad 0.1.0") == "Sanad 0.1.0");
    NF_CHECK(!needs_shaping("Open project file"));
    set_language(Language::English); // restore for other tests
}
