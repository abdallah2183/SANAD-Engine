// UITests — editor string localisation (Phase 15).

#include <NF/Test/TestFramework.hpp>
#include <NF/UI/Localization.hpp>

#include <string>

using namespace nf;
using namespace nf::ui;

NF_TEST(localization_english_is_default) {
    set_language(Language::English);
    NF_CHECK(current_language() == Language::English);
    NF_CHECK(tr("sky") == "Sky");
    NF_CHECK(tr("directional_light") == "Directional Light");
    NF_CHECK(tr("add_sky") == "Add Sky Settings");
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
