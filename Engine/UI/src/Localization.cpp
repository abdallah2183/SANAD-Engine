// NF/UI/Localization.cpp — English source strings + Arabic translations.
//
// Convention: keys are stable snake_case ids; English values are the exact
// strings the editor showed before localisation (so the English UI is
// pixel-identical to before). Arabic values are logical-order UTF-8.

#include <NF/UI/Localization.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace nf::ui {

namespace {

struct Entry {
    const char* key;
    const char* en;
    const char* ar;
};

// Keep sorted by key (deterministic, greppable).
constexpr Entry kEntries[] = {
    {"add_sky", "Add Sky Settings", "إضافة إعدادات السماء"},
    {"apply", "Apply", "تطبيق"},
    {"about", "About", "حول"},
    {"animation", "Animation", "التحريك"},
    {"assets", "Assets", "الأصول"},
    {"audio", "Audio", "الصوت"},
    {"autosave", "Autosave", "الحفظ التلقائي"},
    {"build", "Build", "بناء"},
    {"camera", "Camera", "الكاميرا"},
    {"cancel", "Cancel", "إلغاء"},
    {"cast_shadows", "Cast shadows", "تفعيل الظلال"},
    {"close", "Close", "إغلاق"},
    {"collider", "Collider", "المتصادم"},
    {"color", "Color", "اللون"},
    {"console", "Console", "الطرفية"},
    {"create", "Create", "إنشاء"},
    {"collider", "Collider", "المتصادم"},
    {"color", "Color", "اللون"},
    {"console", "Console", "الطرفية"},
    {"direction", "Direction", "الاتجاه"},
    {"directional_light", "Directional Light", "الإضاءة الاتجاهية"},
    {"enabled", "Enabled", "مفعّل"},
    {"sky", "Sky", "السماء"},
    {"exposure", "Exposure", "التعريض"},
    {"far", "Far", "البعيد"},
    {"file", "File", "ملف"},
    {"game", "Game", "اللعبة"},
    {"gameplay", "Gameplay", "أسلوب اللعب"},
    {"ground", "Ground", "الأرض"},
    {"help", "Help", "مساعدة"},
    {"horizon", "Horizon", "الأفق"},
    {"inspector", "Inspector", "المفتش"},
    {"intensity", "Intensity", "الشدة"},
    {"interval_s", "Interval (s)", "الفاصل (ث)"},
    {"language", "Language", "اللغة"},
    {"load_game", "Load Game", "تحميل اللعبة"},
    {"material", "Material", "الخامة"},
    {"mesh", "Mesh", "المجسم"},
    {"name", "Name", "الاسم"},
    {"near", "Near", "القريب"},
    {"new", "New", "جديد"},
    {"no_save_slots", "no save slots", "لا توجد خانات حفظ"},
    {"no_sky_settings", "No sky settings on this entity.", "لا توجد إعدادات سماء لهذا الكيان."},
    {"no_such_slot", "(no such slot)", "(لا توجد هذه الخانة)"},
    {"nothing_selected", "Nothing selected.", "لا يوجد تحديد."},
    {"open", "Open", "فتح"},
    {"outliner", "Outliner", "شجرة المشهد"},
    {"play", "Play", "تشغيل"},
    {"position", "Position", "الموضع"},
    {"prefab", "Prefab", "القالب الجاهز"},
    {"redo", "Redo", "إعادة"},
    {"rigid_body", "Rigid Body", "الجسم الصلب"},
    {"rotation", "Rotation", "الدوران"},
    {"save", "Save", "حفظ"},
    {"save_as", "Save As...", "حفظ باسم..."},
    {"save_game", "Save Game", "حفظ اللعبة"},
    {"scale", "Scale", "الحجم"},
    {"shadow_bias", "Shadow bias", "انحياز الظل"},
    {"shadow_cascades", "Shadow cascades", "طبقات الظل"},
    {"shadow_distance", "Shadow distance", "مسافة الظل"},
    {"shadow_strength", "Shadow strength", "قوة الظل"},
    {"slot", "Slot", "الخانة"},
    {"stop", "Stop", "إيقاف"},
    {"sun_disk", "Sun disk", "قرص الشمس"},
    {"sun_glow", "Sun glow", "توهج الشمس"},
    {"transform", "Transform", "التحويل"},
    {"undo", "Undo", "تراجع"},
    {"view", "View", "عرض"},
    {"viewport", "Viewport", "منفذ العرض"},
    {"zenith", "Zenith", "السمت"},
};

Language g_language = Language::English;

} // namespace

void set_language(Language lang) {
    g_language = lang;
}

Language current_language() {
    return g_language;
}

std::string tr(const char* key) {
    if (!key) return {};
    for (const auto& e : kEntries) {
        if (std::string(e.key) == key) {
            if (g_language == Language::Arabic) return e.ar;
            return e.en;
        }
    }
    return key;
}

unsigned tr_key_count() {
    unsigned n = 0;
    for (const auto& e : kEntries) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace nf::ui
