// Tests/EditorTests/test_imgui_id_uniqueness.cpp — a lint for the ImGui
// duplicate-ID trap.
//
// The failure this exists for: a `CollapsingHeader("Snap")` section whose first
// control is a `Checkbox("Snap")`. ImGui derives a widget's ID from its label, so
// two visible widgets in one window share an ID, and the symptom is not a wrong
// pixel — it is a modal "MESSAGE FROM DEAR IMGUI Programmer error" popup that the
// user has to dismiss before the editor is usable again, every single time they
// open Settings. It was reported as a screenshot, not as a failed test, because
// NOTHING in the suite could see it: the panels need a GPU, a live EditorApp and
// a real window to run.
//
// So it is checked statically instead. This is not a style lint — it is the only
// place the rule can be enforced, and it turns "a user found it" into "CI found
// it". The rules it encodes are the ones the codebase already follows everywhere
// else, so a hit means a real regression, not a preference.
//
// Two rules, both learned from real reports:
//   1. Inside a `section(...)` / `settings_section(...)` block, no control may
//      reuse the section's label verbatim with no `###` suffix.
//   2. `settings_section` takes a KEY and appends `###sec_<key>`, so section
//      headers are unique by construction. If someone changes it back to taking
//      translated text, the headers collide with their own contents again — and
//      in a different language, since the translated text differs.
//
// Deliberately a regex over source rather than a runtime ImGui pass: a runtime
// pass would need the whole editor running, and would still only cover the
// windows somebody thought to open.
//
// Note on the raw strings: they use an explicit `RE` delimiter because these
// patterns contain the sequence `)"` (a capture group followed by a quote), which
// would terminate a plain R"(...)" at exactly the wrong place.

#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace nf;

namespace {

#ifndef NF_EDITOR_UI_SRC_DIR
    #define NF_EDITOR_UI_SRC_DIR ""
#endif

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> split_lines(const std::string& src) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

std::vector<std::filesystem::path> ui_sources() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path dir(NF_EDITOR_UI_SRC_DIR);
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".cpp") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Plain substring scanning, not <regex>. The patterns here are fixed substrings
// and one quoted key, which find() expresses directly; a regex engine buys
// nothing and costs a heavyweight include plus a pile of escaping in raw
// strings, where a single mistyped `)"` silently ends the literal early.
const char* const kWidgetNames[] = {
    "Checkbox", "Button",   "SmallButton", "SliderFloat", "DragFloat", "DragInt",
    "Combo",    "RadioButton", "ColorEdit3", "ColorEdit4", "InputText", "ListBox",
};

/// If `line` opens a section header, returns its key. Recognises every call form
/// in use — section(AV("k")), section(AV("k").c_str()), settings_section("k"),
/// settings_section("k", true) — by taking the first quoted lowercase token
/// after `section(`.
///
/// Returns an empty string when the header is SCOPED, i.e. the caller passed a
/// bare key and the helper appends its own `###sec_<key>`. Those cannot collide
/// with anything, and reporting them would be a false positive that teaches
/// people to ignore this lint.
std::string section_key_of(const std::string& line) {
    const size_t at = line.find("section(");
    if (at == std::string::npos) {
        return {};
    }
    const size_t q1 = line.find('"', at);
    if (q1 == std::string::npos) {
        return {};
    }
    const size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return {};
    }
    const std::string key = line.substr(q1 + 1, q2 - q1 - 1);
    if (key.empty()) {
        return {};
    }
    // The AV( form means the header's ID IS the translated label, so it can
    // collide. The bare-key form means settings_section scoped it.
    const bool translated = line.find("AV(", at) < line.find_first_of(",)", at + 8);
    if (!translated) {
        return {};
    }
    return key;
}

/// If `line` declares a widget whose visible LABEL is a key and which carries no
/// `###` id, returns the key. The label is detected as AV("k") immediately
/// followed by `.c_str()`, which is the shape that leaves the label as the whole
/// ID; `AV("k") + "##x"` is skipped because the suffix scopes it.
std::string unscoped_widget_key_of(const std::string& line) {
    if (line.find("ImGui::") == std::string::npos) {
        return {};
    }
    bool is_widget = false;
    for (const char* w : kWidgetNames) {
        if (line.find(std::string("ImGui::") + w + "(") != std::string::npos) {
            is_widget = true;
            break;
        }
    }
    if (!is_widget) {
        return {};
    }
    const size_t av = line.find("AV(\"");
    if (av == std::string::npos) {
        return {};
    }
    const size_t q2 = line.find('"', av + 4);
    if (q2 == std::string::npos) {
        return {};
    }
    // Scoped? The id suffix follows the label before .c_str(). ImGui accepts
    // EITHER "###" (label hidden, id only) or "##" (same effect), and this
    // codebase uses "##" for the overwhelming majority of its widgets — so the
    // test is for "##", which also covers "###". Checking only "###" reported
    // every `AV("k") + "##k_id"` as a collision, which is a false positive that
    // would have trained people to ignore this lint.
    const size_t cstr = line.find(".c_str()", av);
    if (cstr == std::string::npos) {
        return {};
    }
    const std::string between = line.substr(av, cstr - av);
    if (between.find("##") != std::string::npos) {
        return {};
    }
    return line.substr(av + 4, q2 - av - 4);
}

} // namespace

NF_TEST(editor_ui_has_no_widget_sharing_its_section_label) {
    const auto files = ui_sources();
    if (files.empty()) {
        // A wrong NF_EDITOR_UI_SRC_DIR would make this lint silently pass, which
        // is worse than it failing. Say so.
        NF_CHECK(!files.empty());
        std::printf("NF_EDITOR_UI_SRC_DIR is not a directory - the duplicate-ID lint "
                    "did not run.\n");
        return;
    }

    std::string problems;
    for (const auto& f : files) {
        const std::string src = slurp(f);
        if (src.empty()) {
            continue;
        }
        const std::string rel = f.filename().string();
        const std::vector<std::string> lines = split_lines(src);
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string key = section_key_of(lines[i]);
            if (key.empty()) {
                continue;
            }
            const size_t end = std::min(lines.size(), i + 40);
            for (size_t j = i + 1; j < end; ++j) {
                if (unscoped_widget_key_of(lines[j]) == key) {
                    problems += "\n  " + rel + ":" + std::to_string(j + 1) + ": widget \"" +
                                key +
                                "\" shares its section header's ID -> add \"##" + key +
                                "\" to the widget's label, or pass the section its key so the"
                                " header scopes its own id";
                }
            }
        }
    }
    if (!problems.empty()) {
        std::printf(
            "ImGui duplicate-ID collisions (each pops a modal error over the editor):%s\n",
            problems.c_str());
    }
    NF_CHECK(problems.empty());
}

NF_TEST(settings_sections_carry_a_stable_key_scoped_id) {
    // settings_section() used to take the TRANSLATED text, so a section header's
    // ImGui ID was the Arabic string in Arabic and the English string in English.
    // Two consequences, both user-visible: the header collided with any control
    // inside it carrying the same label (the modal-error report), and the
    // open/closed state stored in editor_layout.ini did not survive a language
    // switch because the IDs no longer matched.
    const auto files = ui_sources();
    if (files.empty()) {
        NF_SKIP("no UI sources configured");
    }
    bool found_fn = false;
    bool takes_key = false;
    bool scoped_id = false;
    for (const auto& f : files) {
        const std::string src = slurp(f);
        if (src.find("bool settings_section(") == std::string::npos) {
            continue;
        }
        found_fn = true;
        takes_key = takes_key || (src.find("bool settings_section(const char* key") !=
                                  std::string::npos);
        scoped_id = scoped_id || (src.find("\"###sec_\" + key") != std::string::npos);
    }
    if (!found_fn) {
        NF_CHECK(false);
        std::printf("settings_section() not found in the UI sources.\n");
        return;
    }
    if (!takes_key || !scoped_id) {
        std::printf(
            "settings_section() must take a const char* KEY and build \"###sec_\" + key.\n"
            "Taking translated text makes the header ID language-dependent AND lets the\n"
            "header collide with a control of the same label (the modal-error report).\n");
    }
    NF_CHECK(takes_key);
    NF_CHECK(scoped_id);
}
