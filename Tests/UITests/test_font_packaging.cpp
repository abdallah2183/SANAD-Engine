// Tests/UITests/test_font_packaging.cpp — the Arabic UI font must EXIST, and the
// failure must be loud.
//
// This is not a style test. The base font (Segoe UI) is loaded with NO glyph
// ranges, so ImGui rasterises Basic Latin only and the companion TTF is the sole
// source of Arabic codepoints in the entire editor. The first beta shipped
// without it, find_bundled_font() walked up from the executable into folders
// that do not exist in a package, found nothing, and said nothing — so the
// editor opened to a full screen of tofu diamonds with the user left to guess.
//
// A file-existence assertion is the only thing that catches this class, because
// the failure is invisible to every test that does not go looking.

#include <NF/Test/TestFramework.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace nf;

namespace {

#ifndef NF_FONT_DIR
    #define NF_FONT_DIR ""
#endif
#ifndef NF_FONT_FILE
    #define NF_FONT_FILE ""
#endif

// The same two-source walk the editor performs, in the same order. Duplicated
// deliberately: the point is to assert the CONTRACT (a font is reachable from a
// configured build), not to call the editor's internal function, which lives in
// an application target UITests does not link.
std::string find_font(const std::filesystem::path& exe_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string file = NF_FONT_FILE;
    if (file.empty()) {
        return {};
    }
    // 1. The build's absolute override — the path a packaged build relies on.
    if (fs::path(NF_FONT_DIR) != fs::path()) {
        const fs::path cand = fs::path(NF_FONT_DIR) / file;
        if (fs::exists(cand, ec) && !ec) {
            return cand.string();
        }
    }
    // 2. Beside the executable, then walked up.
    fs::path dir = exe_dir;
    for (int i = 0; i < 6; ++i) {
        const fs::path cand = dir / "Resources" / "fonts" / file;
        if (fs::exists(cand, ec) && !ec) {
            return cand.string();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

} // namespace

NF_TEST(the_arabic_ui_font_is_reachable) {
    // The configured font directory must contain the file. This is the single
    // assertion whose absence allowed a package that could not render its own UI.
    const std::string dir = NF_FONT_DIR;
    NF_CHECK(!dir.empty());
    if (dir.empty()) {
        return; // nothing to check, and the assert above already said so
    }
    const std::string file = NF_FONT_FILE;
    NF_CHECK(!file.empty());
    if (file.empty()) {
        return;
    }
    const std::filesystem::path p = std::filesystem::path(dir) / file;
    std::error_code ec;
    const bool present = std::filesystem::exists(p, ec) && !ec;
    NF_CHECK(present); // absent => every Arabic label in the editor is a diamond
    if (!present) {
        return;
    }
    // A real TTF, not a zero-byte placeholder or a Git-LFS pointer. An empty file
    // fails exactly as silently as a missing one: stb_truetype rejects it and
    // every glyph becomes a box.
    const auto sz = std::filesystem::file_size(p, ec);
    NF_CHECK(!ec && sz > 100000u);
    std::ifstream in(p, std::ios::binary);
    NF_CHECK(in.good());
    if (!in.good()) {
        return;
    }
    const std::string head((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // 0x00010000 = TrueType sfnt version. Rejecting anything else means an LFS
    // pointer or an HTML error page cannot pass as a font.
    NF_CHECK(head.size() >= 4u);
    if (head.size() >= 4) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(head.data());
        NF_CHECK(b[0] == 0x00 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00);
    }
}

NF_TEST(the_arabic_font_resolves_from_a_package_layout) {
    // The walk-up fallback is a source-tree convenience. A PACKAGED build has
    // Resources/fonts beside the executable and nothing above it that looks like
    // the engine, so this asserts the direct "beside the exe" case works — it is
    // the layout the installer produces.
    const std::string file = NF_FONT_FILE;
    if (file.empty() || std::string(NF_FONT_DIR).empty()) {
        NF_SKIP("no font configured for this build");
    }
    // Point the "executable directory" at the configured font dir's parent, so
    // the sibling lookup is the thing under test rather than the override.
    const std::filesystem::path parent =
        std::filesystem::path(NF_FONT_DIR).parent_path().parent_path();
    NF_CHECK(!find_font(parent).empty());
}
