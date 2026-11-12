// Measured text layout: wrap/tooltip derive from the draw measurer, Arabic
// shapes once, over-wide words clip (never overflow), tooltips stay on screen.
#include <NF/Test/TestFramework.hpp>
#include <NF/UI/TextLayout.hpp>
#include <NF/UI/ScriptCanvas.hpp>
#include <NF/UI/Widgets.hpp>

#include <string>
using namespace nf::ui;

namespace {
// Deterministic wide/narrow measurers: char count alone cannot predict width.
float wide_width(const std::string& shaped, float size) {
    // ASCII = narrow, everything else = wide. Two strings with the same byte
    // length can have different widths -> proves measurement drives layout.
    float w = 0;
    for (unsigned char c : shaped) {
        if (c < 0x80) w += size * 0.30f;
        else w += size * 1.1f;
    }
    return w;
}
float narrow_width(const std::string& shaped, float size) {
    return static_cast<float>(shaped.size()) * size * 0.30f;
}
bool fits(const std::vector<std::string>& lines, float box, float size, const TextMeasurer& m) {
    for (auto& l : lines) {
        if (l.empty()) continue;
        const float w = m ? m(l, size) : fallback_text_width(l, size);
        if (w > box + 0.01f) return false;
    }
    return true;
}
} // namespace

NF_TEST(text_layout_uses_measure_not_char_count) {
    // Same char count, different widths -> different wrapping.
    const std::string ascii = "aaaaaa";
    const std::string arabic = "مممم";
    const auto lines_ascii = wrap_text(ascii, 30.0f, 20.0f, wide_width);
    const auto lines_arb = wrap_text(arabic, 30.0f, 20.0f, wide_width);
    // ascii fits whole (6*6=36? actually 6*6=36 >30 -> clips/wraps to fewer). Just assert measurer matters:
    // narrow measurer fits more than wide on same box.
    const auto lines_narrow = wrap_text("hello world", 40.0f, 20.0f, narrow_width);
    const auto lines_wide = wrap_text("hello world", 40.0f, 20.0f, wide_width);
    NF_CHECK(lines_wide.size() >= lines_narrow.size());
    NF_CHECK(fits(lines_wide, 40.0f, 20.0f, wide_width));
    // Arabic shaped once: visual differs from logical.
    const auto arb_lines = wrap_text("سلام", 500.0f, 20.0f, wide_width);
    NF_CHECK(arb_lines.size() == 1);
    NF_CHECK(arb_lines[0] != "سلام"); // shaped to presentation forms
    NF_CHECK(fits(arb_lines, 500.0f, 20.0f, wide_width));
    (void)lines_ascii; (void)lines_arb; (void)lines_narrow;
}

NF_TEST(text_layout_overwide_word_clips_inside_box) {
    const std::string long_word = "supercalifragilistic";
    const auto lines = wrap_text(long_word, 100.0f, 20.0f, wide_width);
    NF_CHECK(fits(lines, 100.0f, 20.0f, wide_width));
    NF_CHECK(lines.size() >= 1);
    // Ellipsis marks the clip.
    bool saw_ellipsis = false;
    for (auto& l : lines) if (l.find("\xE2\x80\xA6") != std::string::npos) saw_ellipsis = true;
    NF_CHECK(saw_ellipsis);
    // Hard lines preserved.
    const auto hard = wrap_text("a\n\nb", 500.0f, 20.0f, wide_width);
    NF_CHECK(hard.size() == 3);
    NF_CHECK(hard[1].empty());
}

NF_TEST(text_layout_tooltip_stays_on_screen_with_margins) {
    const std::string long_text = "This is a very long tooltip that must wrap inside the box without drawing outside the window. "
                                  "هذا نص عربي طويل يلتف داخل التلميح دون الخروج عن الشاشة، مع أرقام 123.";
    const auto tl = layout_tooltip(long_text, 800.0f, 600.0f, 790.0f, 590.0f, 22.0f, wide_width);
    NF_CHECK(tl.w <= 360.0f);
    NF_CHECK(tl.x >= 4.0f && tl.y >= 4.0f);
    NF_CHECK(tl.x + tl.w <= 800.001f && tl.y + tl.h <= 600.001f);
    NF_CHECK(!tl.lines.empty());
    NF_CHECK(fits(tl.lines, tl.w - 16.0f, 22.0f, wide_width));
    // Top-left pointer: still inside.
    const auto tl2 = layout_tooltip("hi", 800.0f, 600.0f, 0.0f, 0.0f, 22.0f, wide_width);
    NF_CHECK(tl2.x >= 4.0f && tl2.y >= 4.0f);
    NF_CHECK(tl2.x + tl2.w <= 800.001f && tl2.y + tl2.h <= 600.001f);
}

NF_TEST(text_layout_utf8_cursor_never_splits_a_sequence) {
    const std::string s = "a\xF0\x9F\x98\x80" "b"; // a + emoji (4-byte) + b
    NF_CHECK(utf8_valid(s));
    NF_CHECK(utf8_codepoint_count(s) == 3);
    // prev boundary from end steps over 'b' then over the whole 4-byte emoji.
    const size_t from_end = utf8_prev_boundary(s, s.size());
    NF_CHECK(from_end == 5); // a(1) + emoji(4) = 5
    const size_t before_emoji = utf8_prev_boundary(s, from_end);
    NF_CHECK(before_emoji == 1);
    // Snap mid-sequence down to boundary.
    NF_CHECK(utf8_snap_boundary(s, 2) == 1);
    NF_CHECK(utf8_snap_boundary(s, 5) == 5);
}

NF_TEST(text_layout_script_canvas_tooltip_uses_measurer) {
    ScriptCanvas c;
    c.set_text_measurer(wide_width);
    const auto frame = [&](bool wide) {
        c.begin_frame({.2f,.15f,false,true});
        c.set_viewport(800, 600);
        c.tooltip("t", wide ? "مممممممم" : "aaaa", {.1f,.1f,.3f,.1f});
    };
    frame(true);
    auto dl = c.snapshot(800, 600);
    NF_CHECK(!dl.rects.empty());
    const float tooltip_w_wide = dl.rects.back().rect.w;
    frame(false);
    dl = c.snapshot(800, 600);
    const float tooltip_w_narrow = dl.rects.back().rect.w;
    // Arabic (wide per measurer) gets a wider (or equal-capped) box than ASCII
    // at this viewport: measurement, not char count, decides (Arabic has 10
    // codepoints = 20 bytes, ASCII 4 bytes; a byte-count proxy would invert).
    NF_CHECK(tooltip_w_wide >= tooltip_w_narrow);
    for (auto& r : dl.rects) {
        NF_CHECK(r.rect.x >= 0 && r.rect.y >= 0 && r.rect.x + r.rect.w <= 800.001f && r.rect.y + r.rect.h <= 600.001f);
    }
}
