// EditorTests — Arabic RTL display path (P1: عربي RTL صلب).
//
// Every user-visible string the editor draws goes through the pair
// nf::ui::shape_arabic (Engine/UI/ArabicShaper) + nf::ui::tr
// (Engine/UI/Localization). This file pins that contract at the edge cases the
// panels actually hit — digits and decimal numbers inside an Arabic run,
// brackets in an RTL context, mixed EN/AR lines, combining marks, tatweel —
// plus the live EN/AR toggle the toolbar exposes.
//
// Pure CPU: no GPU, no window, no font atlas.
//
// The two R1 characterisation pins that used to live here (search "R1") have
// been FLIPPED to real expectations: shape_arabic() now keeps a number and the
// punctuation between its digits in one LTR-preserving cluster, so "1.0" no
// longer reads back-to-front inside Arabic. The header of the old pins said to
// flip them when that landed — this is that flip.

#include <NF/Test/TestFramework.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::ui;

namespace {

// UTF-8 encode one codepoint, for building expectations from UCD form tables.
std::string u8c(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string u8s(std::initializer_list<char32_t> cps) {
    std::string out;
    for (char32_t cp : cps) out += u8c(cp);
    return out;
}

// Decodes UTF-8 (the shaper's own decoder is private, and this suite must not
// reach into Engine internals).
bool decode(const std::string& s, std::vector<char32_t>& out) {
    out.clear();
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return true;
}

std::string hex_dump(const std::string& s) {
    std::string out;
    char buf[8];
    for (unsigned char c : s) {
        std::snprintf(buf, sizeof(buf), "%02X ", c);
        out += buf;
    }
    return out;
}

// Equality check that prints both sides as hex: byte-identical Arabic strings
// are unreadable in a terminal, so a bare NF_CHECK_EQ turns a shaping mismatch
// into "false" with no way to see what the shaper actually produced.
void check_shaped(const std::string& got, const std::string& want, int line) {
    if (got != want) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "shaping mismatch at line %d\n  got : %.180s\n  want: %.180s",
                      line, hex_dump(got).c_str(), hex_dump(want).c_str());
        throw std::runtime_error(msg);
    }
}

// The loaded font atlas covers exactly these ranges (see
// Editor/src/ui/UiBackend.cpp arabic_glyph_ranges); the shaper must never emit
// a codepoint outside them or the glyph renders as tofu.
bool in_atlas(char32_t cp) {
    return (cp >= 0x0020 && cp <= 0x00FF) || (cp >= 0x0600 && cp <= 0x06FF) ||
           (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) ||
           (cp >= 0x2010 && cp <= 0x202F) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

// set_language() flips a process-global; every case that touches it restores
// the previous value so the rest of the suite sees the language it started in.
struct LanguageGuard {
    Language prev;
    LanguageGuard() : prev(current_language()) {}
    ~LanguageGuard() { set_language(prev); }
};

} // namespace

NF_TEST(editor_arabic_ascii_passes_through_untouched) {
    check_shaped(shape_arabic(""), "", __LINE__);
    check_shaped(shape_arabic("Save As..."), "Save As...", __LINE__);
    check_shaped(shape_arabic("Gizmo: [W] Translate  [E] Rotate"), "Gizmo: [W] Translate  [E] Rotate",
                 __LINE__);
    NF_CHECK(!needs_shaping("Save"));
    NF_CHECK(!needs_shaping(""));
    NF_CHECK(needs_shaping(u8s({0x062D, 0x0641, 0x0638})));
}

NF_TEST(editor_arabic_digits_inside_arabic_keep_order) {
    // السنة 2024 (logical: ALEF LAM SEEN NOON TEH-MARBUTA SP 2 0 2 4). The digit
    // run is one LTR-preserving cluster, so it lands at the head of the visual
    // string intact, in logical order.
    const std::string got = shape_arabic(u8s({0x0627, 0x0644, 0x0633, 0x0646, 0x0629}) + " 2024");
    const std::string want = "2024 " + u8s({0xFE94, 0xFEE8, 0xFEB4, 0xFEDF, 0xFE8D});
    check_shaped(got, want, __LINE__);
}

NF_TEST(editor_arabic_contiguous_digit_run_pinned) {
    // سنة2024, no space: still one RTL run, digits still in logical order.
    const std::string got = shape_arabic(u8s({0x0633, 0x0646, 0x0629}) + "2024");
    check_shaped(got, "2024" + u8s({0xFE94, 0xFEE8, 0xFEB3}), __LINE__);
}

NF_TEST(editor_arabic_indic_digits_keep_logical_order) {
    // Arabic-Indic digits U+0660..U+0669 are digits too: the run ١٢٣ must not
    // come back as ٣٢١.
    const std::string got =
        shape_arabic(u8s({0x0633, 0x0646, 0x0629, 0x0661, 0x0662, 0x0663}));
    check_shaped(got, u8s({0x0661, 0x0662, 0x0663, 0xFE94, 0xFEE8, 0xFEB3}), __LINE__);
}

NF_TEST(editor_arabic_number_run_keeps_decimal_together) {
    // R1 (COORDINATION) — FIXED. A full stop inside a number run used to shatter
    // the run into separately-reversed clusters, so "الإصدار 1.0" displayed as
    // "الإصدار 0.1". Digits plus the punctuation between them are now one
    // LTR-preserving cluster, so the number survives intact at the head of the
    // visual string.
    // The word also exercises the لإ (lam + alef-hamza-below) ligature, which
    // collapses two logical letters into one presentation codepoint.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0625, 0x0635, 0x062F, 0x0627, 0x0631}) + " 1.0");
    const std::string letters = u8s({0xFEAD, 0xFE8D, 0xFEAA, 0xFEBB, 0xFEF9, 0xFE8D});
    check_shaped(got, "1.0 " + letters, __LINE__);
    NF_CHECK(got.rfind("1.0", 0) == 0);
    NF_CHECK(got.find("0.1") == std::string::npos); // the old defect
}

NF_TEST(editor_arabic_number_run_keeps_ratio_together) {
    // R1 — FIXED, second shape: 16:9 used to come back as 9:16.
    const std::string got =
        shape_arabic(u8s({0x0646, 0x0633, 0x0628, 0x0629}) + " 16:9");
    NF_CHECK(got.rfind("16:9", 0) == 0);
    NF_CHECK(got.find("9:16") == std::string::npos);
}

NF_TEST(editor_arabic_number_run_keeps_leading_sign_together) {
    // R1 — FIXED, third shape: a leading '-' used to become its own cluster, so
    // "السرعة -5" displayed as "السرعة 5-". The scan now starts at a
    // number-punctuation that is immediately followed by a digit, which keeps the
    // sign on the same side as its number.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0633, 0x0631, 0x0639, 0x0629}) + " -5");
    NF_CHECK(got.rfind("-5", 0) == 0);
    NF_CHECK(got.find("5-") == std::string::npos);
}

NF_TEST(editor_arabic_number_run_keeps_internal_hyphen_together) {
    // R1 — FIXED, fourth shape: a range. "الأعمار 1-10" used to display as
    // "الأعمار 01-1" because each digit fragment was reversed on its own.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0623, 0x0639, 0x0645, 0x0627, 0x0631}) + " 1-10");
    NF_CHECK(got.rfind("1-10", 0) == 0);
    NF_CHECK(got.find("01-1") == std::string::npos);
}

NF_TEST(editor_arabic_number_run_stops_at_trailing_punctuation) {
    // The narrowness of the rule matters as much as the fix: a number must END on
    // a digit, so the '.' in "الإصدار 1." stays outside the reversed unit instead
    // of being dragged into it. Sentence punctuation must not glue words together.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0625, 0x0635, 0x062F, 0x0627, 0x0631}) + " 1.");
    NF_CHECK(got.find("1") != std::string::npos);
    // The digit is still there exactly once — nothing was dropped or duplicated.
    NF_CHECK(got.find("11") == std::string::npos);
}

NF_TEST(editor_arabic_brackets_in_rtl_context) {
    // الكلمة (الثانية): brackets are neutrals resolved to the LTR side, so
    // they stay at their logical positions and are NOT mirrored (a full UBA
    // L4 pass would mirror them; documented shaper scope — UI labels, not a
    // text editor). The paren pair still visually wraps الثانية.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0643, 0x0644, 0x0645, 0x0629}) + " (" +
                     u8s({0x062B, 0x0627, 0x0646, 0x064A, 0x0629}) + ")");
    const std::string word1 = u8s({0xFE94, 0xFEE4, 0xFEE0, 0xFEDC, 0xFEDF, 0xFE8D});
    const std::string word2 = u8s({0xFE94, 0xFEF4, 0xFEE7, 0xFE8E, 0xFE9B});
    check_shaped(got, word1 + " (" + word2 + ")", __LINE__);
    NF_CHECK(got.find('(') != std::string::npos);
    NF_CHECK(got.find(')') != std::string::npos);
    // No mirroring: the opener stays '('.
    NF_CHECK(got.find('(') < got.find(')'));
}

NF_TEST(editor_arabic_mixed_latin_arabic_line) {
    // Translate ترجمة: the Latin run is emitted first, untouched — a bidi bug
    // that reversed the whole line would show "etalsnarT".
    const std::string got = shape_arabic(std::string("Translate ") + u8s({0x062A, 0x0631, 0x062C, 0x0645, 0x0629}));
    const std::string word = u8s({0xFE94, 0xFEE4, 0xFE9F, 0xFEAE, 0xFE97});
    check_shaped(got, "Translate " + word, __LINE__);
    NF_CHECK(got.rfind("Translate", 0) == 0);
    // Latin-only and Latin-with-punctuation sub-runs never reverse.
    check_shaped(shape_arabic("ABC abc 1.0 (x)"), "ABC abc 1.0 (x)", __LINE__);
}

NF_TEST(editor_arabic_mixed_line_number_at_end) {
    // "Frame 60" in an Arabic label: the trailing digit run keeps its order.
    const std::string got =
        shape_arabic(u8s({0x0627, 0x0644, 0x0625, 0x0637, 0x0627, 0x0631}) + " 60");
    NF_CHECK(got.rfind("60", 0) == 0);
    NF_CHECK(got.find("06") == std::string::npos);
}

NF_TEST(editor_arabic_marks_stay_with_their_base) {
    // بَرَم (BEH FATHA REH FATHA MEEM): each haraka stays glued to its base
    // through the cluster reversal.
    const std::string got = shape_arabic(u8s({0x0628, 0x064E, 0x0631, 0x064E, 0x0645}));
    check_shaped(got, u8s({0xFEE1, 0xFEAE, 0x064E, 0xFE91, 0x064E}), __LINE__);
}

NF_TEST(editor_arabic_lam_alef_carries_intervening_marks) {
    // لَا (LAM FATHA ALEF): the لا ligature is isolated (nothing connects into
    // it), and the FATHA between the lam and the alef rides after it.
    check_shaped(shape_arabic(u8s({0x0644, 0x064E, 0x0627})), u8s({0xFEFB, 0x064E}), __LINE__);
    // بَلا: the ligature is final (BEH connects into it); the mark still rides.
    check_shaped(shape_arabic(u8s({0x0628, 0x064E, 0x0644, 0x0627})),
                 u8s({0xFEFC, 0xFE91, 0x064E}), __LINE__);
}

NF_TEST(editor_arabic_tatweel_joins_both_sides) {
    // بـب (BEH TATWEEL BEH): the kashida passes through and the neighbours
    // connect through it — initial BEH, final BEH, tatweel between.
    check_shaped(shape_arabic(u8s({0x0628, 0x0640, 0x0628})), u8s({0xFE90, 0x0640, 0xFE91}),
                 __LINE__);
}

NF_TEST(editor_arabic_output_stays_inside_loaded_glyph_ranges) {
    // A codepoint the shaper emits outside the atlas ranges renders as tofu,
    // so every output of every label the editor shows must be coverable.
    const std::vector<std::string> samples = {
        u8s({0x0627, 0x0644, 0x0633, 0x0644, 0x0627, 0x0645}), // السلام
        u8s({0x0645, 0x062D, 0x0631, 0x0643, 0x0633, 0x0648, 0x062F}), // محرك
        std::string("محرك سند (SANAD) v1.0 — 2024"),
        u8s({0x0627, 0x0644, 0x0625, 0x0635, 0x062F, 0x0627, 0x0631}) + " 1.0",
        u8s({0x0628, 0x064E, 0x0631, 0x064E, 0x0645, 0x064F}), // with harakat
    };
    for (const std::string& logical : samples) {
        const std::string visual = shape_arabic(logical);
        std::vector<char32_t> cps;
        NF_CHECK(decode(visual, cps));
        for (char32_t cp : cps) {
            NF_CHECK(in_atlas(cp));
        }
    }
}

NF_TEST(editor_arabic_invalid_utf8_passes_through) {
    const std::string bad("\xFF\xFE broken");
    check_shaped(shape_arabic(bad), bad, __LINE__);
}

NF_TEST(editor_arabic_shaping_is_deterministic) {
    const std::string s = u8s({0x0627, 0x0644, 0x0633, 0x0644, 0x0627, 0x0645});
    check_shaped(shape_arabic(s), shape_arabic(s), __LINE__);
}

NF_TEST(editor_language_toggle_switches_live_without_restart) {
    // The toolbar button calls set_language() mid-session; nothing else has to
    // happen for the next frame to draw the other language.
    LanguageGuard guard;
    set_language(Language::English);
    NF_CHECK(current_language() == Language::English);
    check_shaped(tr("save"), "Save", __LINE__);
    set_language(Language::Arabic);
    NF_CHECK(current_language() == Language::Arabic);
    check_shaped(tr("save"), u8s({0x062D, 0x0641, 0x0638}), __LINE__);
    // Back again, in the same process: no restart, no re-init.
    set_language(Language::English);
    check_shaped(tr("save"), "Save", __LINE__);
    set_language(Language::Arabic);
    check_shaped(tr("save"), u8s({0x062D, 0x0641, 0x0638}), __LINE__);
}

NF_TEST(editor_language_table_has_arabic_for_every_editor_key) {
    // A key with an empty or untranslated Arabic value would silently render
    // English inside the Arabic UI, so every key the panels use must carry a
    // distinct translation. New keys arrive through COORDINATION (R2).
    static const char* const kKeys[] = {
        "about", "add_sky", "add_sky_settings", "animation", "apply", "assets", "audio",
        "autosave", "build",
        "camera", "cancel", "cast_shadows", "close", "collider", "color", "console", "create",
        "direction", "directional_light", "enabled", "exposure", "far", "file", "game",
        "gameplay", "ground", "help", "horizon", "inspector", "intensity", "interval_s",
        "language", "load_game", "material", "mesh", "name", "near", "new", "no_save_slots",
        "no_sky_settings", "no_such_slot", "nothing_selected", "open", "outliner", "play",
        "position", "prefab", "redo", "rigid_body", "rotation", "save", "save_as",
        "save_game", "scale", "shadow_bias", "shadow_cascades", "shadow_distance",
        "shadow_strength", "sky", "slot", "stop", "sun_disk", "sun_glow", "transform",
        "undo", "validation", "view", "viewport", "zenith",
        // G9: Settings > Rendering, and the grid-step slider's visible label.
        "rendering", "no_light_settings", "grid_step_value",
    };
    LanguageGuard guard;
    for (const char* key : kKeys) {
        set_language(Language::English);
        const std::string en = tr(key);
        set_language(Language::Arabic);
        const std::string ar = tr(key);
        NF_CHECK(!en.empty());
        NF_CHECK(!ar.empty());
        NF_CHECK(en != ar); // untranslated would fall through to the English text
    }
}

NF_TEST(editor_language_unknown_key_fails_visible) {
    // Unknown keys return the key itself, never an empty string: a missing
    // translation shows up as raw text in the UI instead of vanishing.
    LanguageGuard guard;
    set_language(Language::English);
    check_shaped(tr("editor_probe_missing_key"), "editor_probe_missing_key", __LINE__);
    set_language(Language::Arabic);
    check_shaped(tr("editor_probe_missing_key"), "editor_probe_missing_key", __LINE__);
}

NF_TEST(editor_language_key_count_never_shrinks) {
    // 70 entries at 2026-09-21 (Phase 15 table). The count only grows — new
    // keys are added through COORDINATION R2 — so a drop here means the table
    // lost entries the editor's localised UI depends on.
    NF_CHECK(tr_key_count() >= 70u);
}

// --- A1: the display path must be SHAPED, never logical ----------------------
//
// The reversal plague. ImGui has no bidi reordering: it draws whatever bytes it
// is handed, left to right. An Arabic label handed over in LOGICAL order (what
// nf::ui::tr returns) therefore renders mirrored and unjoined — the screenshots
// read "فأظشرلأا عوطق" for "إظهار الشبكة" and "زييمتلاا نول" for "لون التمييز",
// while the labels that looked right ("إغلاق", panel headers) all went through
// shape_arabic. Same table, same language, one call apart: the difference is
// exactly tr() vs shape_arabic(tr()).
//
// So the rule the editor now follows is: shape_arabic(tr(key)) for anything a
// widget draws; tr(key) alone only for logic (IDs, comparisons, lookups). These
// two tests pin that the distinction is real and load-bearing, because if the
// two ever produced the same bytes the audit would be pointless and a
// regression back to tr() would be invisible.

NF_TEST(editor_widget_labels_differ_between_logical_and_display_forms) {
    // Every key below is drawn as a widget label somewhere in the panels
    // (Checkbox/DragFloat/Combo/MenuItem/Text/dialog title). For each, the
    // logical form and the display form must genuinely differ, and the display
    // form must be the one carrying Arabic presentation forms — otherwise the
    // font atlas has no glyph and the label paints as disconnected boxes.
    static const char* const kLabelKeys[] = {
        "outliner",   "viewport",    "inspector",  "transform", "material",
        "sky",        "grid_step",   "validation", "local_space", "world_space",
        "snap",       "open_scene",  "save_scene_as", "export", "import_model",
        "sky_preset", "theme_accent", "entities",  "project",   "founder",
        "close",      "move",        "rotate",     "scale_tool",
    };
    LanguageGuard guard;
    set_language(Language::Arabic);
    // Collect every offending key rather than throwing on the first: fixing the
    // dictionary one key per build is how the untranslated-UI class survived.
    std::string problems;
    for (const char* key : kLabelKeys) {
        const std::string logical = tr(key);
        const std::string display = shape_arabic(logical);

        auto report = [&](const char* why) {
            char msg[320];
            std::snprintf(msg, sizeof(msg), "\n  key '%s': %s (logical=%s)", key, why,
                          hex_dump(logical).c_str());
            problems += msg;
            return false;
        };

        // Arabic, so the distinction is meaningful for this key. A key missing
        // from the Arabic dictionary falls back to its English text, which is
        // the untranslated-UI defect A3 tracks — caught here because an
        // English label can never be a shaping case.
        if (!needs_shaping(logical)) {
            report("no Arabic entry: fell back to English");
            continue;
        }
        // Shaping is not a no-op here: this is the difference the audit is about.
        if (logical == display) {
            report("shape_arabic() is a no-op for this string");
            continue;
        }

        std::vector<char32_t> lc;
        std::vector<char32_t> dc;
        NF_CHECK(decode(logical, lc));
        NF_CHECK(decode(display, dc));

        // Logical order = base Arabic block (U+0600..U+06FF).
        bool logical_has_base = false;
        for (char32_t cp : lc) {
            if (cp >= 0x0620 && cp <= 0x06FF) logical_has_base = true;
        }
        if (!logical_has_base) {
            report("logical form carries no base Arabic letters");
            continue;
        }

        // Display order = presentation forms (U+FE70..U+FEFF), which is what the
        // loaded atlas covers. A label drawn from `logical` instead shows
        // isolated, mirrored letters.
        bool display_has_presentation = false;
        for (char32_t cp : dc) {
            if (cp >= 0xFE70 && cp <= 0xFEFF) display_has_presentation = true;
        }
        if (!display_has_presentation) {
            report("display form carries no presentation forms");
            continue;
        }
    }
    if (!problems.empty()) {
        throw std::runtime_error("widget labels are not display-safe:" + problems);
    }
}

NF_TEST(editor_shaped_label_reverses_run_order_for_visual_display) {
    // A two-word Arabic label in logical order is W1 SP W2. Because the shaper
    // emits visual order, the whole label must equal the second word shaped,
    // then the separator, then the first word shaped. That reversal is precisely
    // what a tr()-only label fails to do, and it is the mechanism behind the
    // mirrored screenshots.
    //
    // Built from codepoints rather than a translation so the test does not
    // depend on the wording of any particular key.
    const std::string w1 = u8s({0x0627, 0x0644, 0x0634, 0x0628, 0x0643, 0x0629}); // الشبكة
    const std::string w2 = u8s({0x0625, 0x0637, 0x0641, 0x0627, 0x0621});         // إطفاء

    const std::string logical = w1 + " " + w2;
    const std::string display = shape_arabic(logical);

    NF_CHECK(display != logical);                       // it really did reorder
    check_shaped(display, shape_arabic(w2) + " " + shape_arabic(w1), __LINE__);
    // And the un-shaped string is the WRONG one to hand a widget: its word order
    // is the reverse of what the display form produces.
    NF_CHECK(display != w2 + " " + w1);
}

// --- A3b: the inspector/console labels that never called AV() ----------------
//
// A3's audit diffed the keys the editor ALREADY passed to AV() against the
// table, so it was structurally blind to the other half of the problem: ~60 raw
// Latin literals sitting directly in widget positions that never called AV() at
// all. Nothing looked them up, so no key was ever "missing" and the diff stayed
// clean while the inspector rendered English. Those are converted now (same rule
// as A1), and this is the set that conversion introduced.
//
// Each must carry REAL Arabic. A value that fell back to English renders Latin
// inside the Arabic UI, which is the exact defect A3 exists to prevent.

NF_TEST(editor_a3b_labels_carry_real_arabic) {
    static const char* const kKeys[] = {
        "about_tagline",   "active",        "albedo",         "allow_sleep",
        "angular_damping", "ao",            "apply_to_prefab", "asset_id",
        "assign",          "autoplay",      "base_color",     "category",
        "clear",           "clip",          "console_debug",  "console_error",
        "console_info",    "console_trace", "console_warn",   "emission",
        "emission_strength", "entity_prefix", "error",        "fov",
        "friction",        "half_extents",  "import",         "import_to",
        "level",           "linear_damping", "loop",          "looping",
        "mass",            "max_distance",  "metallic",       "min_distance",
        "mip_filter",      "module",        "module_no_props", "module_no_state",
        "navigate_hint",   "no_clips",      "no_gameplay_modules", "normal",
        "overwrite",       "parent_prefix", "paused",         "pitch",
        "radius",          "restitution",   "revert_to_prefab", "roughness",
        "save_material",   "save_trace",    "shape",          "shift_click_multi",
        "speed",           "state_machine", "transform_space", "type",
        "volume",
    };

    // These three legitimately keep Latin inside the Arabic, and it is a
    // deliberate decision rather than a missed translation:
    //   navigate_hint     names physical keys (WASD / QE) — the letters printed
    //                     on the keyboard are Latin whatever the UI language is;
    //   shift_click_multi names a modifier chord the same way;
    //   no_clips          quotes the literal `Animation:` token from the scene
    //                     file format, which an author has to grep for verbatim.
    static const char* const kLatinAllowed[] = {"navigate_hint", "shift_click_multi",
                                                "no_clips"};

    auto allows_latin = [&](const char* key) {
        for (const char* k : kLatinAllowed) {
            if (std::string(k) == key) {
                return true;
            }
        }
        return false;
    };

    LanguageGuard guard;
    std::string problems;
    for (const char* key : kKeys) {
        set_language(Language::English);
        const std::string en = tr(key);
        set_language(Language::Arabic);
        const std::string ar = tr(key);

        if (en == ar) {
            problems += std::string("\n  '") + key + "': Arabic falls back to English";
            continue;
        }
        if (!needs_shaping(ar)) {
            problems += std::string("\n  '") + key + "': value contains no Arabic at all";
            continue;
        }
        if (allows_latin(key)) {
            continue;
        }
        // A half-translated value is the likeliest regression, so assert there is
        // no Latin LETTER left. Digits, punctuation and %s are all fine.
        for (char c : ar) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                problems += std::string("\n  '") + key + "': Arabic value contains Latin text";
                break;
            }
        }
    }
    if (!problems.empty()) {
        throw std::runtime_error("A3b labels are not fully localised:" + problems);
    }
}
