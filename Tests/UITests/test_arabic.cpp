// UITests — Arabic shaping + bidi (Phase 15).
//
// Expected codepoints come from the Unicode Character Database forms table
// (the same source the shaper's table was generated from). Pure CPU.

#include <NF/Test/TestFramework.hpp>
#include <NF/UI/ArabicShaper.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::ui;

namespace {

// UTF-8 encode one codepoint for building expectations.
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

} // namespace

NF_TEST(arabic_passthrough_when_no_shaping_needed) {
    NF_CHECK(!needs_shaping("Hello, world!"));
    NF_CHECK(!needs_shaping(""));
    NF_CHECK(shape_arabic("Hello") == "Hello");
    NF_CHECK(shape_arabic("") == "");
    NF_CHECK(needs_shaping("hello سلام"));
}

NF_TEST(arabic_single_letter_is_isolated) {
    // BEH alone -> isolated form.
    NF_CHECK(shape_arabic(u8s({0x0628})) == u8s({0xFE8F}));
    // ALEF alone -> isolated.
    NF_CHECK(shape_arabic(u8s({0x0627})) == u8s({0xFE8D}));
}

NF_TEST(arabic_bab_shapes_initial_final) {
    // باب (BEH ALEF BEH), visual order: isolated BEH, final ALEF, initial BEH.
    // (The last BEH is isolated: alef never connects forward.)
    NF_CHECK(shape_arabic(u8s({0x0628, 0x0627, 0x0628})) == u8s({0xFE8F, 0xFE8E, 0xFE91}));
}

NF_TEST(arabic_alsalam_full_word) {
    // السلام: ALEF LAM SEEN LAM ALEF MEEM. The LAM+ALEF pair forms the
    // لا ligature (final, since SEEN connects into it). Visual order:
    // isolated MEEM, final لا, medial SEEN, initial LAM, isolated ALEF.
    NF_CHECK(shape_arabic(u8s({0x0627, 0x0644, 0x0633, 0x0644, 0x0627, 0x0645})) ==
             u8s({0xFEE1, 0xFEFC, 0xFEB4, 0xFEDF, 0xFE8D}));
}

NF_TEST(arabic_medial_forms_without_ligatures) {
    // شمس (SHEEN MEEM SEEN): initial + medial + final.
    NF_CHECK(shape_arabic(u8s({0x0634, 0x0645, 0x0633})) == u8s({0xFEB2, 0xFEE4, 0xFEB7}));
    // نور (NOON WAW REH): initial + final (waw has no medial) + isolated.
    NF_CHECK(shape_arabic(u8s({0x0646, 0x0648, 0x0631})) == u8s({0xFEAD, 0xFEEE, 0xFEE7}));
}

NF_TEST(arabic_lam_alef_ligatures) {
    // لا alone -> isolated ligature.
    NF_CHECK(shape_arabic(u8s({0x0644, 0x0627})) == u8s({0xFEFB}));
    // بلا: BEH initial + ligature final, reversed.
    NF_CHECK(shape_arabic(u8s({0x0628, 0x0644, 0x0627})) == u8s({0xFEFC, 0xFE91}));
    // لأ (hamza above) and لإ (hamza below) ligatures exist.
    NF_CHECK(shape_arabic(u8s({0x0644, 0x0623})) == u8s({0xFEF7}));
    NF_CHECK(shape_arabic(u8s({0x0644, 0x0625})) == u8s({0xFEF9}));
}

NF_TEST(arabic_mixed_latin_runs_keep_order) {
    // "aب": Latin run passes through, Arabic run shapes.
    NF_CHECK(shape_arabic(std::string("a") + u8s({0x0628})) == std::string("a") + u8s({0xFE8F}));
    // "XالسلامY": outer Latin stays outside, Arabic reversed in the middle.
    const std::string visual =
        shape_arabic(std::string("X") + u8s({0x0627, 0x0644, 0x0633, 0x0644, 0x0627, 0x0645}) + "Y");
    const std::string expect =
        std::string("X") + u8s({0xFEE1, 0xFEFC, 0xFEB4, 0xFEDF, 0xFE8D}) + "Y";
    NF_CHECK(visual == expect);
}

NF_TEST(arabic_digits_keep_logical_order) {
    // سنة2024: digits ride inside the RTL run but keep their order.
    const std::string visual =
        shape_arabic(u8s({0x0633, 0x0646, 0x0629}) + std::string("2024"));
    // Visual = "2024" first (reversed run), then shaped letters reversed.
    NF_CHECK(visual.rfind("2024", 0) == 0); // starts with 2024
}

NF_TEST(arabic_harakat_ride_with_base) {
    // بَ (BEH + FATHA): isolated BEH followed by the mark, unchanged order.
    NF_CHECK(shape_arabic(u8s({0x0628, 0x064E})) == u8s({0xFE8F, 0x064E}));
}

NF_TEST(arabic_invalid_utf8_passes_through) {
    const std::string bad("\xFF\xFE invalid");
    NF_CHECK(shape_arabic(bad) == bad);
}

NF_TEST(arabic_classifiers) {
    NF_CHECK(is_arabic_letter(0x0628));
    NF_CHECK(is_arabic_letter(0x0640)); // tatweel
    NF_CHECK(!is_arabic_letter(U'a'));
    NF_CHECK(is_arabic_mark(0x064E));
    NF_CHECK(!is_arabic_mark(0x0628));
    NF_CHECK(is_digit(U'5'));
    NF_CHECK(is_digit(0x0663));
    NF_CHECK(is_rtl_char(0x0628));
    NF_CHECK(!is_rtl_char(U'a'));
}
