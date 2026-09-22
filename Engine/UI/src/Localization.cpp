// NF/UI/Localization.cpp — English source strings + Arabic translations.
//
// Convention: keys are stable snake_case ids; English values are the exact
// strings the editor showed before localisation (so the English UI is
// pixel-identical to before). Arabic values are logical-order UTF-8.
//
// Duplicate-key cleanup: the rows for collider/color/console (x2), create,
// file, help, language, game, load_game, save_game, slot and autosave used to
// appear TWICE each. Lookup is first-match-wins, so the second copy was dead
// weight that silently shadowed nothing and confused every audit — the repeat
// is deleted and the surviving row keeps the single shared meaning (e.g. one
// "create" for the menu and the button; they always meant the same verb).

#include <NF/UI/Localization.hpp>

#include <NF/Core/Logger.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace nf::ui {

namespace {

struct Entry {
    const char* key;
    const char* en;
    const char* ar;
};

// Keep sorted by key within each block (deterministic, greppable).
constexpr Entry kEntries[] = {
    {"add_sky_settings", "Add Sky Settings", "إضافة إعدادات السماء"},
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
    // A4/A3: the outliner's Delete button and the Edit-menu Delete item were
    // raw Latin literals with no key at all, so the Arabic UI showed "Delete".
    {"delete", "Delete", "حذف"},
    {"direction", "Direction", "الاتجاه"},
    {"directional_light", "Directional Light", "الإضاءة الاتجاهية"},
    {"enabled", "Enabled", "مفعّل"},
    // A3: the outliner's empty-scene hint was a raw English literal.
    {"empty_scene_hint",
     "Empty scene — press + Empty to create your first entity, or drag a mesh from the "
     "Asset Browser into the viewport.",
     "مشهد فارغ — اضغط + فراغ لإنشاء أول كيان، أو اسحب مجسمًا من متصفح الأصول إلى منفذ العرض."},
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

    // --- Phase 21 additions: menus, toolbar, settings, export/import --------
    // Appended as one block rather than re-sorted, so the diff of the natural
    // sky / Create menu / Export work stays readable. Keys stay unique.
    {"add_camera", "Add Camera", "إضافة كاميرا"},
    {"add_cube", "Add Cube", "إضافة مكعب"},
    {"add_ground", "Add Ground Plane", "إضافة أرضية"},
    {"add_light", "Add Directional Light", "إضافة إضاءة اتجاهية"},
    {"add_quad", "Add Quad", "إضافة مستطيل"},
    {"add_sky", "Add Sky", "إضافة سماء"},
    {"add_sphere", "Add Sphere", "إضافة كرة"},
    {"assets_count", "assets", "أصل"},
    {"autosave_interval", "Autosave interval (s)", "فاصل الحفظ التلقائي (ث)"},
    {"browse", "Browse...", "استعراض..."},
    {"clear_imports", "Clear finished imports", "مسح الاستيرادات المنتهية"},
    {"edit", "Edit", "تحرير"},
    {"english", "English", "الإنجليزية"},
    {"entities", "entities", "كيان"},
    {"export", "Export", "تصدير"},
    {"export_format", "Format", "الصيغة"},
    {"export_hint", "Exports the selection (or the whole scene) as one mesh. ",
                    "يصدّر التحديد (أو المشهد كاملاً) كمجسم واحد. "},
    {"export_scope", "Scope", "النطاق"},
    {"export_selection", "Selected meshes", "المجسمات المحددة"},
    {"export_whole_scene", "Whole scene", "المشهد كاملاً"},
    {"filter", "Filter", "تصفية"},
    {"grid_step", "Grid step", "خطوة الشبكة"},
    {"import_model", "Import model...", "استيراد مجسم..."},
    {"import_source", "Import source", "مصدر الاستيراد"},
    {"local_space", "Local", "محلي"},
    {"move", "Move", "تحريك"},
    {"new_scene", "New Scene", "مشهد جديد"},
    {"no_grid", "No grid", "بلا شبكة"},
    {"open_scene", "Open Scene...", "فتح مشهد..."},
    {"output_path", "Output file", "ملف الإخراج"},
    {"preferences", "Preferences...", "التفضيلات..."},
    {"rotate", "Rotate", "تدوير"},
    {"save_prefab", "Save as prefab", "حفظ كقالب"},
    {"save_scene", "Save Scene", "حفظ المشهد"},
    {"save_scene_as", "Save Scene As...", "حفظ المشهد باسم..."},
    {"scale_tool", "Scale", "تحجيم"},
    {"settings", "Settings", "الإعدادات"},
    {"sky_day", "Clear day", "نهار صافٍ"},
    {"sky_night", "Night", "ليل"},
    {"sky_preset", "Sky preset", "نمط السماء"},
    {"sky_sunset", "Golden hour", "وقت الغروب"},
    {"snap", "Snap", "محاذاة"},
    {"theme_accent", "Accent colour", "لون التمييز"},
    // A1/A3: the toolbar draws AV("validation") in the status bar and as a
    // View/Settings checkbox label. It had no entry at all, so both rendered the
    // raw key in English inside the Arabic UI — an untranslated label that no
    // screenshot could explain as a shaping bug.
    {"validation", "Validation", "التحقق"},
    {"viewport_hint_drag", "Drag the selection to move it (W/E/R switch mode, Esc cancels).",
                           "اسحب التحديد لنقله (W/E/R لتغيير الأداة، Esc للإلغاء)."},
    {"viewport_hint_multi", "%zu selected - drag moves them all (one undo step).",
                            "المحدد: %zu - السحب ينقلها كلها (خطوة تراجع واحدة)."},
    {"viewport_hint_none", "No selection - gizmo disabled. Click the scene or an outliner row.",
                           "لا يوجد تحديد - الأداة معطلة. انقر في المشهد أو في شجرة المشهد."},
    {"viewport_hint_dragging", "Dragging %zu entr%s - release to commit, Esc cancels.",
                               "جارٍ سحب %zu كيان - أفلت للتثبيت، Esc للإلغاء."},
    {"world_space", "World", "عالمي"},

    // --- A3b: inspector fields, panel buttons and console labels -------------
    // The A3 key-table diff could not see these: they were raw Latin literals in
    // widget positions that never called AV() at all, so nothing looked them up
    // and no key was ever missed. Same rule as A1 — a label a widget draws goes
    // through AV() — but the audit had to be a source sweep, not a key diff.
    {"about_tagline", "The Modern Extensible Arabic C++23 & Vulkan Game Engine",
                      "محرك الألعاب العربي الحديث القابل للتوسّع"},
    {"active", "Active", "مفعّل"},
    {"albedo", "Albedo", "البياض"},
    {"allow_sleep", "Allow Sleep", "السماح بالسكون"},
    {"angular_damping", "Angular Damping", "التخميد الزاوي"},
    {"ao", "AO", "الإحاطة"},
    {"apply_to_prefab", "Apply to prefab", "تطبيق على القالب"},
    {"asset_id", "AssetId", "معرّف الأصل"},
    {"assign", "Assign", "تعيين"},
    {"base_color", "Base color", "اللون الأساسي"},
    {"category", "Category", "الفئة"},
    {"clear", "Clear", "مسح"},
    {"clip", "Clip", "المقطع"},
    // Console level filter. The CATEGORY list stays English on purpose: it
    // mirrors the LogCategory enum, and translating it would break the
    // correspondence with the category tag on every log line.
    {"console_debug", "Debug", "التنقيح"},
    {"console_error", "Error", "خطأ"},
    {"console_info", "Info", "معلومات"},
    {"console_trace", "Trace", "تتبع"},
    {"console_warn", "Warn", "تحذير"},
    {"emission", "Emission", "الانبعاث"},
    {"emission_strength", "Emission strength", "شدة الانبعاث"},
    {"entity_prefix", "Entity", "كيان"},
    {"error", "Error", "خطأ"},
    {"fov", "FOV", "مجال الرؤية"},
    {"friction", "Friction", "الاحتكاك"},
    {"half_extents", "Half Extents", "نصف الأبعاد"},
    {"import", "Import", "استيراد"},
    {"import_to", "Import to", "الاستيراد إلى"},
    {"level", "Level", "المستوى"},
    {"linear_damping", "Linear Damping", "التخميد الخطي"},
    {"loop", "Loop", "التكرار"},
    {"mass", "Mass", "الكتلة"},
    {"metallic", "Metallic", "المعدنية"},
    {"mip_filter", "Mip filter", "مرشح الميب"},
    {"module_no_props", "module declares no editable properties",
                        "الوحدة لا تعرّف خصائص قابلة للتحرير"},
    {"navigate_hint", "Navigate: right-drag orbits, wheel zooms, right-hold + WASD/QE flies.",
                      "التنقل: السحب الأيمن يدوّر، العجلة تقرّب، الاستمرار الأيمن مع WASD/QE للطيران."},
    {"no_clips", "no clips (see the Animation: line in the scene file)",
                 "لا توجد مقاطع (انظر سطر Animation: في ملف المشهد)"},
    {"normal", "Normal", "الناظم"},
    {"overwrite", "Overwrite", "الكتابة فوق"},
    {"parent_prefix", "Parent", "الأب"},
    {"paused", "Paused", "متوقف مؤقتًا"},
    {"radius", "Radius", "نصف القطر"},
    {"restitution", "Restitution", "الارتداد"},
    {"revert_to_prefab", "Revert to prefab", "استعادة من القالب"},
    {"roughness", "Roughness", "الخشونة"},
    {"save_material", "Save material", "حفظ الخامة"},
    {"save_trace", "Save Chrome Trace", "حفظ ملف التتبع"},
    {"shape", "Shape", "الشكل"},
    {"shift_click_multi", "shift-click = multi", "shift-click = متعدد"},
    {"speed", "Speed", "السرعة"},
    {"state_machine", "State machine", "آلة الحالات"},
    {"transform_space", "Transform space", "فضاء التحويل"},
    {"type", "Type", "النوع"},
    // Audio / gameplay inspector fields and their hints.
    {"volume", "Volume", "مستوى الصوت"},
    {"pitch", "Pitch", "التردد"},
    {"looping", "Looping", "متكرر"},
    {"autoplay", "Autoplay", "تشغيل تلقائي"},
    {"min_distance", "Min distance", "أدنى مسافة"},
    {"max_distance", "Max distance", "أقصى مسافة"},
    {"module", "Module", "الوحدة"},
    {"no_gameplay_modules", "no gameplay modules are registered in this build",
                           "لا توجد وحدات أسلوب لعب مسجلة في هذا البناء"},
    {"module_no_state", "module declares no reflected state",
                        "الوحدة لا تعرّف حالة معكوسة"},
    {"attach", "Attach", "إرفاق"},
    {"detach", "Detach", "فصل"},

    // Names that are Arabic in BOTH languages: the engine's own name and the
    // project lead's. Keeping them in the table (rather than in the panel code)
    // is what lets the UI layer stay free of non-ASCII literals.
    {"arabic_name", "العربية", "العربية"},
    {"engine_name", "SANAD Engine", "محرك سند"},
    {"founder", "Abdallah", "عبدالله"},
    {"profiler", "Profiler", "المُحلِّل"},
    {"new_project", "New Project...", "مشروع جديد..."},
    {"project", "Project", "المشروع"},

    // --- Game-Ready G3 additions: runtime game UI (HUD/menus) ---------------
    // Appended as one block (same convention as Phase 21): no re-sorting of
    // earlier blocks, keys checked unique against the whole table (duplicate
    // keys silently shadow — first-match linear scan).
    {"main_menu", "Main Menu", "القائمة الرئيسية"},
    {"new_game", "New Game", "لعبة جديدة"},
    {"resume", "Resume", "استئناف"},
    {"restart", "Restart", "إعادة المحاولة"},
    {"quit", "Quit", "خروج"},
    {"game_over", "Game Over", "انتهت اللعبة"},
    {"you_died", "You Died", "لقد مُتّ"},
    {"health", "Health", "الصحة"},
    {"ammo", "Ammo", "الذخيرة"},
    {"back", "Back", "رجوع"},
    {"master_volume", "Master Volume", "الصوت العام"},
    {"music_volume", "Music Volume", "مستوى الموسيقى"},
    {"sfx_volume", "SFX Volume", "مستوى المؤثرات"},
    {"sensitivity", "Sensitivity", "الحساسية"},
    {"dialogue_continue", "Continue", "متابعة"},
    {"press_to_start", "Press Enter to Start", "اضغط Enter للبدء"},
    {"score", "Score", "النقاط"},
    {"coins", "Coins", "العملات"},

    // --- Game-Ready G9 additions: editor Settings > Rendering ----------------
    // Appended as one block (same convention as Phase 21 / G3): no re-sorting of
    // earlier blocks, keys checked unique against the whole table (duplicate
    // keys silently shadow — first-match linear scan).
    {"rendering", "Rendering", "العرض"},
    {"no_light_settings", "No light in this scene.", "لا توجد إضاءة في هذا المشهد."},
    // The grid SIZE slider in Settings carried only "###grid_step" — an ID with
    // no visible label. Same key family as "grid_step" ("Grid"), but the two
    // widgets must not share a label or ImGui would give them one ID.
    {"grid_step_value", "Grid step", "خطوة الشبكة"},

    // --- Arabic-shell additions: the Win32 launcher + editor literal sweep --
    // The launcher (Editor/src/ProjectLauncher.cpp) is a hand-drawn GDI shell:
    // every literal below used to be a hardcoded English string, so picking
    // Arabic in Settings repainted nothing. All shell text now goes through
    // tr() (logic) + shape_arabic() (display), exactly like the editor.
    // sh_ = shell/launcher, err_ = project-name validation reasons.
    {"sh_nav_projects", "Projects", "المشاريع"},
    {"sh_nav_new", "New project", "مشروع جديد"},
    {"sh_nav_learn", "Learn", "تعلّم"},
    {"sh_nav_store", "Asset Store", "متجر الأصول"},
    {"sh_nav_settings", "Settings", "الإعدادات"},
    {"sh_nav_help", "Help", "مساعدة"},
    {"sh_projects_title", "Projects", "المشاريع"},
    {"sh_projects_sub", "Open a project or start a new one.", "افتح مشروعًا أو ابدأ مشروعًا جديدًا."},
    {"sh_new_title", "Create New Project", "إنشاء مشروع جديد"},
    {"sh_learn_title", "Learning Sanad", "تعلّم سند"},
    {"sh_help_title", "Sanad Help", "مساعدة سند"},
    {"sh_settings_title", "Sanad Settings", "إعدادات سند"},
    {"sh_store_title", "Sanad Asset Store", "متجر أصول سند"},
    {"sh_store_sub", "Curated packages — opening soon.", "حزم مختارة — الافتتاح قريبًا."},
    {"sh_btn_open_file", "Open project file", "فتح ملف مشروع"},
    {"sh_btn_new_project", "+ New project", "+ مشروع جديد"},
    {"sh_btn_open_project", "Open project", "فتح المشروع"},
    {"sh_btn_show_folder", "Show in folder", "عرض في المجلد"},
    {"sh_btn_create", "Create Project", "إنشاء المشروع"},
    {"sh_btn_cancel", "Cancel", "إلغاء"},
    {"sh_btn_browse", "Browse…", "استعراض…"},
    {"sh_btn_clear_recent", "Clear recent list", "مسح قائمة المشاريع الأخيرة"},
    {"sh_btn_docs", "Docs", "المستندات"},
    {"sh_btn_bug", "Bug report", "الإبلاغ عن خطأ"},
    {"sh_search_placeholder", "Search projects", "ابحث في المشاريع"},
    {"sh_hero_engine", "Engine %s   Renderer %s", "المحرك %s   العارض %s"},
    {"sh_hero_edited", "Edited %s", "آخر تحرير %s"},
    {"sh_version", "Sanad %s", "سند %s"},
    {"sh_windows_only", "Windows PC only", "ويندوز فقط"},
    {"sh_tagline", "Game engine", "محرك ألعاب"},
    {"sh_all_projects", "All projects %zu", "كل المشاريع %zu"},
    {"sh_no_match", "No projects match this search.", "لا توجد مشاريع تطابق هذا البحث."},
    {"sh_soon", "soon", "قريبًا"},
    {"sh_tpl_nature", "Nature Exploration", "استكشاف الطبيعة"},
    {"sh_tpl_platformer", "Core Platformer", "منصات أساسي"},
    {"sh_tpl_arena", "FPS Arena", "ساحة تصويب"},
    {"sh_tpl_side", "Side-Scroller Starter", "بداية التمرير الجانبي"},
    {"sh_tpl_blank", "Blank Scene", "مشهد فارغ"},
    {"sh_tpl_marine", "Marine Environment", "بيئة بحرية"},
    {"sh_setup", "Project Setup", "إعداد المشروع"},
    {"sh_name_label", "Project Name:", "اسم المشروع:"},
    {"sh_loc_label", "Project Location:", "موقع المشروع:"},
    {"sh_renderer_label", "Target Renderer:", "العارض المستهدف:"},
    {"sh_renderer_value", "Vulkan (fixed — the engine is Vulkan-only)",
                          "فولكان (ثابت — المحرك فولكان فقط)"},
    {"sh_create_project", "Create Project", "إنشاء المشروع"},
    {"sh_general", "General Preferences", "التفضيلات العامة"},
    {"sh_applies", "Applies to the editor session", "ينطبق على جلسة المحرر"},
    {"sh_defaults", "Project Defaults", "إعدادات المشاريع الافتراضية"},
    {"sh_default_loc", "Default location: Documents", "الموقع الافتراضي: المستندات"},
    {"sh_renderer_fixed", "Renderer: Vulkan (fixed)", "العارض: فولكان (ثابت)"},
    {"sh_editor_iface", "Editor & Interface", "المحرر والواجهة"},
    {"sh_theme_fixed", "Theme: forge dark (fixed)", "السمة: داكنة (ثابتة)"},
    // Language names render in their own script in BOTH languages (a language
    // name is shown as its speakers write it), so en == ar here on purpose —
    // the uniqueness/non-empty tests exempt these two keys explicitly.
    {"sh_radio_en", "English", "English"},
    {"sh_radio_ar", "العربية (Arabic)", "العربية (Arabic)"},
    {"sh_cat_chars", "3D Characters", "شخصيات ثلاثية الأبعاد"},
    {"sh_cat_textures", "PBR Textures", "خامات فيزيائية"},
    {"sh_cat_vfx", "VFX Packs", "حزم المؤثرات"},
    {"sh_cat_gui", "GUI Kits", "أطقم الواجهة"},
    {"sh_cat_audio", "Music & Audio", "الموسيقى والصوت"},
    {"sh_cat_plugins", "Plugins", "الإضافات"},
    {"sh_cat_opens", "Category opens with the store", "يُفتتح القسم مع المتجر"},
    {"sh_learn_academy", "Sanad Academy", "أكاديمية سند"},
    {"sh_learn_academy_sub", "Structured courses and the 20-minute tutorial.",
                             "دورات منظمة ودرس تعريفي في 20 دقيقة."},
    {"sh_learn_script", "Visual Scripting Guide", "دليل البرمجة المرئية"},
    {"sh_learn_script_sub", "Node scripting — coming with the visual system.",
                            "برمجة العقد — قادمة مع النظام المرئي."},
    {"sh_learn_pipeline", "Asset Pipeline", "خط إنتاج الأصول"},
    {"sh_learn_pipeline_sub", "Importing and managing 2D/3D content.",
                              "استيراد وإدارة المحتوى ثنائي وثلاثي الأبعاد."},
    {"sh_learn_api", "SanadScript API", "واجهة سند سكربت"},
    {"sh_learn_api_sub", "Full reference for Lua and C# gameplay.",
                         "مرجع كامل للعب بلغة لوا وسي شارب."},
    {"sh_learn_physics", "Physics & Collision", "الفيزياء والتصادم"},
    {"sh_learn_physics_sub", "Jolt bodies, characters, vehicles, destruction.",
                             "أجسام جولت والشخصيات والمركبات والتدمير."},
    {"sh_learn_gfx", "Advanced Graphics", "الرسوميات المتقدمة"},
    {"sh_learn_gfx_sub", "PBR, shadows, and the render graph.", "التظليل الفيزيائي والظلال ومخطط العرض."},
    {"sh_help_docs", "Documentation", "التوثيق"},
    {"sh_help_docs_sub", "Complete reference in the Docs folder.", "مرجع كامل في مجلد المستندات."},
    {"sh_help_tuts", "Tutorials & Guides", "الدروس والأدلة"},
    {"sh_help_tuts_sub", "Step-by-step guides in Docs.", "أدلة خطوة بخطوة في المستندات."},
    {"sh_help_forum", "Community Forum", "منتدى المجتمع"},
    {"sh_help_forum_sub", "Meet other developers — online soon.", "قابل مطورين آخرين — قريبًا عبر الإنترنت."},
    {"sh_help_notes", "Release Notes", "ملاحظات الإصدار"},
    {"sh_help_notes_sub", "What changed (ROADMAP.md).", "ما الجديد (ROADMAP.md)."},
    {"sh_help_support", "Support Tickets", "تذاكر الدعم"},
    {"sh_help_support_sub", "Formal support channel — online soon.", "قناة دعم رسمية — قريبًا عبر الإنترنت."},
    {"sh_help_feature", "Feature Requests", "طلبات الميزات"},
    {"sh_help_feature_sub", "Vote on ideas — online soon.", "صوّت على الأفكار — قريبًا عبر الإنترنت."},
    // Status-line messages. The trailing-colon rows are PREFIXES: the path is
    // appended after a newline, so the bidi run of the Latin path stays intact
    // (shape_arabic keeps Latin runs in order inside an Arabic line).
    {"sh_st_missing", "That project file no longer exists:", "ملف المشروع لم يعد موجودًا:"},
    {"sh_st_choose_loc", "Choose a location for the project.", "اختر موقعًا للمشروع."},
    {"sh_st_exists", "A folder with that name already exists:", "يوجد مجلد بهذا الاسم بالفعل:"},
    {"sh_st_create_fail", "Could not create the project:", "تعذر إنشاء المشروع:"},
    {"sh_st_nfproj_missing", "The project was created but its .nfproj is missing:",
                             "أُنشئ المشروع لكن ملف .nfproj مفقود:"},
    {"sh_st_docs_missing", "Docs folder not found beside this build.",
                           "لم يُعثر على مجلد المستندات بجانب هذا البناء."},
    {"sh_st_script_soon", "Visual scripting arrives with the node system.",
                          "البرمجة المرئية قادمة مع نظام العقد."},
    {"sh_st_roadmap_missing", "ROADMAP.md not found beside this build.",
                              "لم يُعثر على ROADMAP.md بجانب هذا البناء."},
    {"sh_st_community_soon", "Online community opens with the Asset Store.",
                             "يُفتتح المجتمع عبر الإنترنت مع متجر الأصول."},
    {"sh_st_cleared", "Recent list cleared.", "مُسحت قائمة المشاريع الأخيرة."},
    // --- Shell upgrade: empty library, template blurbs, confirm/drop/remove --
    {"sh_empty_title", "No projects yet.", "لا توجد مشاريع بعد."},
    {"sh_empty_hint", "Create your first project to get started.", "أنشئ مشروعك الأول للبدء."},
    {"sh_tpl_nature_desc", "Open-world foliage and terrain sample.", "عيّنة عالم مفتوح بعشب وتضاريس."},
    {"sh_tpl_platformer_desc", "Side-view jumping and platforms.", "قفز ومنصات بعرض جانبي."},
    {"sh_tpl_arena_desc", "First-person arena combat.", "قتال ساحة بمنظور أول."},
    {"sh_tpl_side_desc", "2D side-scroller starter.", "بداية لعبة تمرير جانبي ثنائية الأبعاد."},
    {"sh_tpl_blank_desc", "Empty scene with the default setup.", "مشهد فارغ بالإعداد الافتراضي."},
    {"sh_tpl_marine_desc", "Underwater world and vehicles.", "عالم تحت الماء ومركبات."},
    {"sh_confirm_clear_title", "Clear recent list", "مسح القائمة الأخيرة"},
    {"sh_confirm_clear_text", "Forget all recent projects? This cannot be undone.",
                              "نسيان كل المشاريع الأخيرة؟ لا يمكن التراجع."},
    {"sh_st_drop_only", "Drop a single .nfproj file to open it.", "أفلت ملف .nfproj واحد لفتحه."},
    {"sh_st_removed", "Removed from the recent list.", "أُزيل من قائمة المشاريع الأخيرة."},
    {"sh_name_hint", "MyGame", "لعبتي"},
    {"sh_grid_more", "+ %zu more", "+ %zu أخرى"},
    // Native dialog titles: passed LOGICAL (the OS dialog shapes natively).
    {"sh_dialog_open_project", "Open project", "فتح مشروع"},
    {"sh_dialog_choose_loc", "Choose a location for the new project", "اختر موقعًا للمشروع الجديد"},
    // "Edited N units ago" buckets for the project cards (edited_ago()).
    {"sh_ago_now", "just now", "الآن"},
    {"sh_ago_minutes", "%lld minutes ago", "منذ %lld دقيقة"},
    {"sh_ago_hours", "%lld hours ago", "منذ %lld ساعة"},
    {"sh_ago_days", "%lld days ago", "منذ %lld يوم"},
    {"sh_ago_weeks", "%lld weeks ago", "منذ %lld أسبوع"},
    {"sh_ago_months", "%lld months ago", "منذ %lld شهر"},
    // Project-name validation reasons (were English sentences in the header).
    {"err_name_empty", "Enter a project name.", "أدخل اسم المشروع."},
    {"err_name_long", "Name is too long (64 characters max).", "الاسم طويل جدًا (64 حرفًا كحد أقصى)."},
    {"err_name_fragment", "That is a path fragment, not a name.", "هذا جزء مسار وليس اسمًا."},
    {"err_name_edges", "Name cannot start or end with a space or a dot.",
                       "لا يمكن أن يبدأ الاسم أو ينتهي بمسافة أو نقطة."},
    {"err_name_forbidden", "Name contains a character Windows forbids in a path.",
                           "يحتوي الاسم على حرف يمنعه ويندوز في المسارات."},
    {"err_name_control", "Name contains a control character.", "يحتوي الاسم على حرف تحكم."},
    {"err_name_reserved", "Name is a reserved Windows device name.", "الاسم محجوز كاسم جهاز في ويندوز."},

    // --- Editor literal sweep: user-facing Latin literals that never had keys.
    // Debug/profiler readouts (viewport lit=, Frame/GPU/Render CPU/Memory lines,
    // import [id] log lines, dropped=, level/category tags, content:// paths)
    // stay English on purpose: they are technical diagnostics, not UI text.
    {"fps", "FPS", "إطار/ث"},
    {"create_empty", "+ Empty", "+ فارغ"},
    {"console_search_hint", "search text", "نص البحث"},
    {"spatial_3d", "3D (spatial)", "ثلاثي الأبعاد (مكاني)"},
    {"anim_bones", "bones: %d", "العظام: %d"},
    {"anim_procedural", "procedural: %s (%s, %.2fs)", "إجرائي: %s (%s, %.2f ث)"},
    {"anim_time_state", "time: %.3fs  state: %s", "الزمن: %.3f ث  الحالة: %s"},
    {"audio_buffer", "buffer: %s%s", "المخزن: %s%s"},
    {"audio_generated", "(generated)", "(مُوَلَّد)"},
    {"audio_no_data", "  [no data]", "  [لا بيانات]"},
    {"audio_cursor", "cursor: %d frames, playing: %s", "المؤشر: %d إطار، الحالة: %s"},
    {"yes", "yes", "نعم"},
    {"no", "no", "لا"},
    {"disabled", "Disabled", "معطّل"},
    {"audio_duration", "duration: %.2fs (%u Hz, %u ch)", "المدة: %.2f ث (%u هرتز، %u قناة)"},
    {"gfx_attached", "attached: %s (%s)", "مرفق: %s (%s)"},
    {"module_not_registered", "module '%s' is not registered in this build",
                              "الوحدة '%s' غير مسجلة في هذا البناء"},
    {"none", "None", "بلا"},
    {"nearest", "Nearest", "الأقرب"},
    {"linear", "Linear", "خطّي"},
    {"ping_pong", "Ping-Pong", "ذهاب وإياب"},
    {"spin", "spin", "دوران"},
    {"bob", "bob", "تمايل"},
    {"assets_count_fmt", "%zu assets", "%zu من الأصول"},
    {"console_dropped", "dropped=%zu", "الملغاة=%zu"},
    {"about_engine_tree", "(engine tree)", "(شجرة المحرك)"},
    {"import_state_queued", "queued", "في الانتظار"},
    {"import_state_working", "working", "جارٍ العمل"},
    {"import_state_failed", "failed", "فشل"},
    {"import_state_done", "done", "اكتمل"},
    // Rigid-body / collider combo items (inspector) and the asset-browser
    // type filter: widget labels, so they translate like everything else.
    // Order matters — the combo index maps straight onto the enum.
    {"rb_static", "Static", "ثابت"},
    {"rb_dynamic", "Dynamic", "متحرك"},
    {"rb_kinematic", "Kinematic", "حركي"},
    {"col_sphere", "Sphere", "كرة"},
    {"col_box", "Box", "صندوق"},
    {"col_plane", "Plane", "مستوٍ"},
    {"asset_type_all", "All", "الكل"},
    {"asset_type_mesh", "Mesh", "مجسم"},
    {"asset_type_texture", "Texture", "نقشة"},
    {"asset_type_material", "Material", "الخامة"},
    {"asset_type_shader", "Shader", "مظلل"},
    {"asset_type_scene", "Scene", "مشهد"},
    {"asset_no_preview", "[no preview]", "[لا معاينة]"},
    {"inspector_root", "(root)", "(الجذر)"},
};

Language g_language = Language::English;

// tr() runs per widget per frame, so lookup is a hash map built once — NOT a
// linear scan. First row wins on duplicates (same rule the old scan had), so a
// repeated key can never silently change meaning; the duplicate tests prove the
// table itself carries no repeats.
const std::unordered_map<std::string_view, const Entry*>& tr_map() {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string_view, const Entry*>();
        m->reserve(sizeof(kEntries) / sizeof(kEntries[0]) * 2);
        for (const auto& e : kEntries) {
            // emplace (not operator[]): the FIRST row keeps the slot.
            m->emplace(std::string_view(e.key), &e);
        }
        return m;
    }();
    return *map;
}

// One-time miss log: a missing key is a content bug worth exactly one log line,
// not one per frame per widget.
void log_missing_once(const std::string& key) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    const std::lock_guard<std::mutex> lock(mutex);
    if (logged.insert(key).second) {
        NF_LOG_WARN(nf::LogCategory::Core, "Localization: missing key '{}'", key);
    }
}

} // namespace

void set_language(Language lang) {
    g_language = lang;
}

Language current_language() {
    return g_language;
}

std::string tr(const char* key) {
    if (key == nullptr) {
        return {};
    }
    const auto it = tr_map().find(std::string_view(key));
    if (it == tr_map().end()) {
        // Unknown key: fail visible (the key itself), never empty.
        log_missing_once(key);
        return key;
    }
    const Entry* e = it->second;
    if (g_language == Language::Arabic && e->ar != nullptr && e->ar[0] != '\0') {
        return e->ar;
    }
    if (e->en != nullptr && e->en[0] != '\0') {
        return e->en;
    }
    // Empty value in both languages: still never empty.
    log_missing_once(key);
    return key;
}

unsigned tr_key_count() {
    return static_cast<unsigned>(tr_map().size());
}

unsigned tr_table_count() {
    return static_cast<unsigned>(sizeof(kEntries) / sizeof(kEntries[0]));
}

const char* tr_table_key_at(unsigned index) {
    const unsigned n = tr_table_count();
    if (index >= n) {
        return nullptr;
    }
    return kEntries[index].key;
}

unsigned tr_duplicate_count() {
    // Rows beyond the first per key: zero means the table is clean.
    return tr_table_count() - tr_key_count();
}

} // namespace nf::ui
