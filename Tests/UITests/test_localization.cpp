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

// Keys whose Arabic value is deliberately identical to English (a language
// name is shown in its own script in both languages).
bool same_in_both_languages(const std::string& key) {
    return key == "arabic_name" || key == "sh_radio_en" || key == "sh_radio_ar";
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
        "sh_help_notes_sub",      // ROADMAP.md is a filename quoted verbatim
        "sh_st_roadmap_missing",  // same filename, status-line wording
        "sh_st_nfproj_missing",   // .nfproj is the extension quoted verbatim
        "sh_st_drop_only",        // same extension, drop-target hint
        "sh_radio_ar",            // own-script tag "العربية (Arabic)"
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
