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
    {"add_day_night", "Add Day/Night Cycle", "إضافة دورة ليل ونهار"},
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
    {"day_length", "Day Length (s)", "طول اليوم (ثانية)"},
    {"drive_light", "Drive Sun Light", "قيادة ضوء الشمس"},
    {"hour", "Hour", "الساعة"},
    {"near", "Near", "القريب"},
    {"new", "New", "جديد"},
    {"no_day_night", "No day/night cycle on this entity.", "لا توجد دورة ليل ونهار لهذا الكيان."},
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
    // Scene audio environment: reverb zones, the level's music and its
    // ambience bed. Each of the three is an entity component with its own
    // section, so each needs its own title, empty-state line and add button.
    {"reverb_zone", "Reverb Zone", "منطقة الصدى"},
    {"no_reverb_zone", "No reverb zone on this entity.", "لا توجد منطقة صدى على هذا الكيان."},
    {"add_reverb_zone", "Add Reverb Zone", "إضافة منطقة صدى"},
    {"inner_radius", "Inner Radius", "نصف القطر الداخلي"},
    {"wet_gain", "Wet Gain", "مستوى البلل"},
    {"decay", "Decay (s)", "التلاشي (ث)"},
    {"predelay", "Pre-delay (s)", "التأخير المسبق (ث)"},
    {"echo_spacing", "Echo Spacing (s)", "تباعد الصدى (ث)"},
    {"music", "Music", "الموسيقى"},
    {"no_music", "No music on this entity.", "لا توجد موسيقى على هذا الكيان."},
    {"add_music", "Add Music", "إضافة موسيقى"},
    {"music_buffer", "Music Buffer", "مخزن الموسيقى"},
    {"ambience", "Ambience", "الجو المحيط"},
    {"no_ambience", "No ambience on this entity.", "لا توجد خلفية جو على هذا الكيان."},
    {"add_ambience", "Add Ambience", "إضافة خلفية جو"},
    {"ambience_buffer", "Ambience Buffer", "مخزن خلفية الجو"},
    {"fade_in", "Fade In (s)", "التلاشي الداخلي (ث)"},
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
    {"sh_version", "Version %s", "الإصدار %s"},
    // The product name. Identical in both languages on purpose (same rule as
    // the language radio tags and `deg_fmt`): a product name is not translated.
    // This key exists so the shell has ONE brand string instead of a hardcoded
    // Arabic literal in one place and "SANAD" in the window title, the file
    // filter and the window class.
    {"sh_brand", "SANAD", "SANAD"},
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
    {"asset_type_script", "Script", "سكربت"},
    {"asset_no_preview", "[no preview]", "[لا معاينة]"},
    {"inspector_root", "(root)", "(الجذر)"},
    // Phase 24: Lua script inspector (path field, file creation, live readout).
    {"script", "Script", "السكربت"},
    {"script_path", "Script path", "مسار السكربت"},
    {"new_script", "New script", "سكربت جديد"},
    {"script_no_data", "  [no script]", "  [لا سكربت]"},
    {"script_loaded", "loaded: %d bytes, %d lines", "المحمّل: %d بايت، %d سطر"},
    // Phase 25: wiring panels (destruction, audio buffer, particles, cloth,
    // character). Display strings only — logic keys stay unshaped.
    {"destructible", "Destructible", "القابل للتدمير"},
    {"no_destructible", "No destructible on this entity.", "لا يوجد جسم قابل للتدمير لهذا الكيان."},
    {"no_rigid_body", "No rigid body on this entity.", "لا يوجد جسم صلب لهذا الكيان."},
    {"no_collider", "No collider on this entity.", "لا يوجد متصادم لهذا الكيان."},
    {"chunks", "Chunks", "الشظايا"},
    {"seed", "Seed", "البذرة"},
    {"strength", "Strength", "القوة"},
    {"damage_threshold", "Damage threshold", "عتبة الضرر"},
    {"blast_radius", "Blast radius", "نصف قطر الانفجار"},
    {"buffer", "Buffer", "المخزن"},
    {"attach_buffer", "Attach buffer", "إرفاق مخزن"},
    {"particles", "Particles", "الجسيمات"},
    {"no_particles", "No particle emitter on this entity.", "لا يوجد باعث جسيمات لهذا الكيان."},
    {"rate", "Rate", "المعدل"},
    {"lifetime", "Lifetime", "العمر"},
    {"lifetime_spread", "Lifetime spread", "تفاوت العمر"},
    {"velocity", "Velocity", "السرعة"},
    {"spread", "Spread", "التفاوت"},
    {"gravity", "Gravity", "الجاذبية"},
    {"drag", "Drag", "المقاومة"},
    {"start_size", "Start size", "الحجم الابتدائي"},
    {"end_size", "End size", "الحجم النهائي"},
    {"start_color", "Start color", "اللون الابتدائي"},
    {"end_color", "End color", "اللون النهائي"},
    {"max_particles", "Max particles", "أقصى الجسيمات"},
    {"cloth", "Cloth", "القماش"},
    {"no_cloth", "No cloth on this entity.", "لا يوجد قماش لهذا الكيان."},
    {"res_x", "Res X", "الدقة الأفقية"},
    {"res_z", "Res Z", "الدقة العمقية"},
    {"spacing", "Spacing", "التباعد"},
    {"damping", "Damping", "التخميد"},
    {"stiffness", "Stiffness", "الصلابة"},
    {"iterations", "Iterations", "التكرارات"},
    {"substeps", "Substeps", "الخطوات الفرعية"},
    {"character", "Character", "الشخصية"},
    {"no_character", "No character on this entity.", "لا توجد شخصية لهذا الكيان."},
    {"max_speed", "Max speed", "السرعة القصوى"},
    {"acceleration", "Acceleration", "التسارع"},
    {"air_control", "Air control", "التحكم الهوائي"},
    {"jump_speed", "Jump speed", "سرعة القفز"},
    {"slope_limit", "Slope limit", "حد المنحدر"},
    {"wish_dir", "Wish direction", "اتجاه الحركة"},
    {"jump", "Jump", "قفزة"},
    {"drive", "Drive", "قيادة"},
    {"grounded_yes", "grounded: yes", "على الأرض: نعم"},
    {"grounded_no", "grounded: no", "ليست على الأرض"},
    // Phase 28: navigation (voxel navmesh volume + walkers). Same rule as the
    // panels above — display strings only, logic keys stay unshaped. Keys that
    // already existed ("speed", "radius", "enabled", "apply", "attach",
    // "detach") are deliberately NOT repeated here: a second row would trip the
    // duplicate audit for no change in behaviour.
    {"navmesh", "NavMesh", "شبكة التنقل"},
    {"no_navmesh", "No navigation volume on this entity.",
     "لا يوجد حجم تنقل لهذا الكيان."},
    {"nav_agent", "Nav Agent", "عميل التنقل"},
    {"no_nav_agent", "No nav agent on this entity.", "لا يوجد عميل تنقل لهذا الكيان."},
    {"nav_area", "Area extents", "أبعاد الحجم"},
    {"cell_size", "Cell size", "حجم الخلية"},
    {"cell_height", "Cell height", "ارتفاع الخلية"},
    {"slope", "Slope", "الميل"},
    {"climb", "Climb", "الصعود"},
    {"headroom", "Headroom", "ارتفاع النفاذ"},
    {"min_area", "Min region area", "أدنى مساحة منطقة"},
    {"agent_radius", "Agent radius", "نصف قطر العميل"},
    {"jump_distance", "Jump distance", "مسافة القفز"},
    {"jump_height", "Jump height", "ارتفاع القفز"},
    {"max_verts", "Max verts", "أقصى عدد رؤوس"},
    {"goal", "Goal", "الهدف"},
    {"arrive_radius", "Arrive radius", "نصف قطر الوصول"},
    {"navmesh_polys", "NavMesh polygons", "مضلعات شبكة التنقل"},
    {"nav_agents", "Nav agents", "عملاء التنقل"},
    {"nav_agents_arrived", "%d / %d agents at goal", "%d / %d عميل وصل للهدف"},
    // External IDE (Unity-style script editing): one button, three backends.
    {"open_in_vs", "Open in VS", "فتح في فيجوال ستوديو"},
    // Richer UI: outliner search is keyless (reuses "filter"), but the
    // inspector Add menu and the viewport simulation overlay need labels.
    {"add_component", "Add Component", "إضافة مكون"},
    {"stats_entities", "%zu entities", "%zu من الكيانات"},
    {"stats_scripts", "%d / %d scripts", "%d / %d من السكربتات"},
    {"stats_particles", "%zu particles (%zu emitters)", "%zu جسيمات (%zu باعث)"},
    {"stats_cloths", "%zu cloths", "%zu قماش"},
    {"stats_characters", "%zu characters", "%zu شخصيات"},
    {"stats_ai", "%zu AI actors", "%zu ممثل ذكاء"},
    // Unity-style Project panel: roots, folders, navigation.
    {"browser_content", "Content", "المحتوى"},
    {"browser_project", "Project files", "ملفات المشروع"},
    {"browser_root", "Root", "الجذر"},
    {"browser_up", "Up", "أعلى"},
    {"folder", "Folder", "مجلد"},
    {"folder_name", "Folder name", "اسم المجلد"},
    {"new_folder", "New Folder", "مجلد جديد"},
    // --- Console / Outliner polish -----------------------------------------
    {"autoscroll", "Autoscroll", "تمرير تلقائي"},
    {"selected_count", "%zu selected", "%zu محدد"},
    // --- FileSystem dock (Godot-style): favorites / history / icons / grid --
    {"filesystem", "FileSystem", "نظام الملفات"},
    {"history", "History", "السجل"},
    {"favorites", "Favorites", "المفضلة"},
    {"folders", "Folders", "المجلدات"},
    {"files", "Files", "الملفات"},
    {"filter_files", "Filter Files", "تصفية الملفات"},
    // "back" and "open_scene" are NOT repeated here: both already exist in
    // earlier blocks with the same wording, and a second row is dead weight
    // that trips tr_duplicate_count() (lookup is first-match-wins, so the copy
    // never changed anything except the duplicate audit).
    {"forward", "Forward", "تقدم"},
    {"refresh", "Refresh", "تحديث"},
    {"view_list", "List", "قائمة"},
    {"view_grid", "Grid", "شبكة"},
    {"add_favorite", "Add to favorites", "إضافة للمفضلة"},
    {"remove_favorite", "Remove from favorites", "إزالة من المفضلة"},
    {"show_in_folder", "Copy path", "نسخ المسار"},
    {"empty_folder_hint", "Empty folder — drag meshes here or create a script.",
     "مجلد فارغ — اسحب المجسمات هنا أو أنشئ سكربت."},
    {"no_files_match", "No files match the filter.", "لا توجد ملفات تطابق التصفية."},
    {"folders_count_fmt", "%zu folders", "%zu مجلد"},
    {"files_count_fmt", "%zu files", "%zu ملف"},
    {"create_menu", "New...", "جديد..."},
    {"collapse_all", "Collapse", "طي"},
    // --- Asset delete / rename (FileSystem dock context menus) --------------
    {"delete_asset", "Delete", "حذف الأصل"},
    {"delete_folder", "Delete folder", "حذف المجلد"},
    {"rename_asset", "Rename...", "إعادة تسمية..."},
    {"delete_confirm_title", "Delete", "حذف"},
    {"delete_confirm", "Delete '%s'?", "حذف «%s»؟"},
    {"delete_confirm_folder",
     "Delete the folder '%s' and everything inside it? This cannot be undone.",
     "حذف المجلد «%s» وكل ما بداخله؟ لا يمكن التراجع."},
    {"rename_prompt", "New name", "الاسم الجديد"},
    {"name_no_spaces", "Name cannot contain spaces.", "الاسم لا يمكن أن يحتوي مسافات."},
    {"deleted_fmt", "Deleted: %s", "تم الحذف: %s"},
    {"renamed_fmt", "Renamed: %s", "تمت إعادة التسمية: %s"},
    // --- View framing (viewport toolbar + Home key) -------------------------
    {"frame_selection", "Frame", "تحديد الإطار"},
    {"frame_all", "Frame All", "تحديد الكل"},
    {"frame_selection_hint", "Centre the view on the selection (Home)",
     "توسيط العرض على المحدد (Home)"},
    // The Arabic companion font failed to load. Every Arabic label is a tofu
    // diamond at this point, so the message has to name the FILE and the folder,
    // not just say something is wrong. Shown as a top banner, not only logged.
    {"err_font_missing_ar",
     "Arabic font not found: copy Resources/fonts next to SANADEditor.exe "
     "(every Arabic label is currently unreadable).",
     "الخط العربي غير موجود: انسخ مجلد Resources/fonts بجوار SANADEditor.exe "
     "(جميع النصوص العربية غير مقروءة حالياً)."},
    {"frame_all_hint", "Centre the view on everything in the scene (Shift+Home)",
     "توسيط العرض على كل شيء في المشهد (Shift+Home)"},

    // --- UI/UX overhaul: status bar + viewport orientation gizmo -------------
    // Appended as one block (same convention as Phase 21 / G3 / G9): no
    // re-sorting of earlier blocks, keys checked unique against the whole table
    // (duplicate keys silently shadow — first-match linear scan).
    {"status_editing", "Editing", "وضع التحرير"},
    {"status_playing", "Playing", "قيد التشغيل"},
    {"status_saved", "Saved", "محفوظ"},
    {"status_unsaved", "Unsaved changes — press Ctrl+S to save.",
                       "تغييرات غير محفوظة — اضغط Ctrl+S للحفظ."},
    {"status_ok", "No validation errors", "لا أخطاء تحقق"},
    {"status_errors", "%u validation errors", "%u أخطاء تحقق"},
    {"untitled", "Untitled", "بلا عنوان"},
    {"axis_gizmo_hint", "World axes — the current view orientation.",
                        "محاور العالم — اتجاه العرض الحالي."},
    // The keyboard reference window (Help > Keyboard shortcuts, F1).
    {"shortcuts", "Keyboard shortcuts", "اختصارات لوحة المفاتيح"},
    // View > Reset layout: back to the built-in dock arrangement.
    {"reset_layout", "Reset layout", "إعادة تعيين التخطيط"},

    // --- Dedicated editors: one panel per engine function --------------------
    // Appended as one block (same convention as Phase 21 / G3 / G9): no
    // re-sorting of earlier blocks, keys checked unique against the whole table
    // (duplicate keys silently shadow — first-match linear scan).
    {"panel_lighting", "Lighting", "الإضاءة"},
    // Tab-sized labels. A dock tab shares a ~250px bar with three siblings, so
    // the definite article ("الإضاءة") costs ~10px each and four tabs stop
    // fitting — the Window menu keeps the full names.
    {"tab_lighting", "Lighting", "إضاءة"},
    {"tab_environment", "Environment", "بيئة"},
    {"tab_camera", "Camera", "كاميرا"},
    {"tab_render", "Performance", "أداء"},
    {"tab_world", "World", "عالم"},
    {"panel_environment", "Environment", "البيئة"},
    {"panel_camera", "Camera", "الكاميرا"},
    {"panel_render", "Render & Performance", "العرض والأداء"},
    {"panel_world", "World", "العالم"},
    {"window", "Window", "نافذة"},
    {"sun", "Sun", "الشمس"},
    {"azimuth", "Azimuth", "الزاوية الأفقية"},
    {"elevation", "Elevation", "الارتفاع"},
    {"deg_fmt", "%.0f°", "%.0f°"},
    {"shadows", "Shadows", "الظلال"},
    {"presets", "Presets", "أنماط جاهزة"},
    {"time_of_day", "Time of day", "وقت اليوم"},
    {"clock", "Clock", "الساعة"},
    {"palette", "Palette", "لوحة الألوان"},
    {"clear_color", "Clear colour", "لون الخلفية"},
    {"lens", "Lens", "العدسة"},
    {"aspect", "Aspect", "نسبة الأبعاد"},
    {"target", "Target", "الهدف"},
    {"performance", "Performance", "الأداء"},
    {"statistics", "Statistics", "الإحصاءات"},
    {"frame_time", "Frame", "الإطار"},
    {"gpu_time", "GPU", "المعالج الرسومي"},
    {"gpu_avg", "GPU (avg)", "المعالج الرسومي (متوسط)"},
    {"cull_time", "Cull", "الاستبعاد"},
    {"draw_prep", "Draw prep", "تحضير الرسم"},
    {"draw_calls", "Draw calls", "نداءات الرسم"},
    {"visible_objects", "Visible", "المرئية"},
    {"assets_cached", "Assets cached", "أصول مخزّنة"},
    {"scene_load", "Last load", "آخر تحميل"},
    {"rhi_objects", "Live RHI objects", "كائنات RHI الحية"},
    {"census", "Census", "الإحصاء"},
    {"meshes", "Meshes", "المجسمات"},
    {"lights", "Lights", "الأضواء"},
    {"cameras", "Cameras", "الكاميرات"},
    {"skies", "Skies", "إعدادات السماء"},
    {"rigid_bodies", "Rigid bodies", "أجسام صلبة"},
    {"colliders", "Colliders", "متصادمات"},
    {"destructibles", "Destructibles", "قابلة للتدمير"},
    {"animations", "Animations", "تحريكات"},
    {"cloths", "Cloths", "أقمشة"},
    {"characters", "Characters", "شخصيات"},
    {"live_systems", "Live systems", "الأنظمة الحية"},
    {"scripts", "Scripts", "السكربتات"},
    {"particles_alive", "Particles", "الجسيمات"},
    {"ai_actors", "AI actors", "ممثلو الذكاء"},
    {"selection", "Selection", "التحديد"},
    {"primary", "Primary", "الأساسي"},
    {"scene", "Scene", "المشهد"},
    {"status", "Status", "الحالة"},
    {"bound_to", "Bound to", "مرتبط بـ"},
    {"lighting_none_hint", "No directional light in this scene — the renderer falls back to its own default. Add one to author the lighting.",
     "لا توجد إضاءة اتجاهية في هذا المشهد — يستخدم العارض الإعداد الافتراضي. أضف واحدة لتحرير الإضاءة."},
    {"environment_none_hint", "No sky in this scene — the renderer paints its cold-start sky. Add one to author the environment.",
     "لا توجد سماء في هذا المشهد — يرسم العارض سماء البداية. أضف واحدة لتحرير البيئة."},
    {"camera_none_hint", "No camera in this scene. Add one so the game has a viewpoint to render from.",
     "لا توجد كاميرا في هذا المشهد. أضف واحدة ليكون للعبة منظور تُعرض منه."},
    {"shadow_distance_hint", "0 = use the camera's far plane. Lower values concentrate the shadow atlas on the range a player can read.",
     "٠ = استخدام المستوى البعيد للكاميرا. القيم الأقل تركّز خرائط الظل على المدى الذي يراه اللاعب."},
    {"time_of_day_hint", "Blends the three sky presets through a full day. Drag to preview live, then Apply.",
     "يمزج أنماط السماء الثلاثة عبر يوم كامل. اسحب للمعاينة المباشرة ثم طبّق."},
    {"camera_transform_hint", "Position and rotation come from the entity's Transform, which is also what the viewport camera follows.",
     "الموضع والدوران من تحويل الكيان، وهو ما تتبعه كاميرا منفذ العرض أيضًا."},
    // --- Post-processing stack (design §206) --------------------------------
    {"post_process", "Post-processing", "المعالجة اللاحقة"},
    {"no_post_process", "No post-processing on this entity. The renderer's defaults apply.",
     "لا توجد معالجة لاحقة لهذا الكيان. تُستخدم الإعدادات الافتراضية للعارض."},
    {"add_post_process", "Add post-processing", "إضافة معالجة لاحقة"},
    {"post_process_hint", "Every stage is off by default and an off stage costs nothing. Bloom records a real blur chain; the rest run inside the tonemap pass.",
     "كل مرحلة متوقفة افتراضيًا والمرحلة المتوقفة لا تكلف شيئًا. الوهج يشغّل سلسلة تمويه حقيقية، والبقية تعمل داخل تمريرة تعيين الدرجات."},
    {"lens_effects", "Lens effects", "تأثيرات العدسة"},
    {"distortion", "Distortion", "التشوّه"},
    {"chromatic_aberration", "Chromatic aberration", "الزيغ اللوني"},
    {"depth_of_field", "Depth of field", "عمق الميدان"},
    {"focus_distance", "Focus distance", "مسافة التركيز"},
    {"focus_range", "Focus range", "مدى التركيز"},
    {"blur_radius", "Blur radius", "نصف قطر التمويه"},
    {"motion_blur", "Motion blur", "ضبابية الحركة"},
    {"max_length", "Max length", "أقصى طول"},
    {"tonemap", "Tonemap", "تعيين الدرجات"},
    {"tonemap_not_set", "Not set (renderer default)", "غير محدد (افتراضي العارض)"},
    {"tonemap_exponential", "Exponential", "أسي"},
    {"tonemap_aces", "ACES", "ACES"},
    {"tonemap_reinhard", "Reinhard", "Reinhard"},
    {"tonemap_linear", "Linear", "خطي"},
    {"renderer_default", "(renderer default)", "(افتراضي العارض)"},
    {"color_lut", "Color LUT", "جدول الألوان"},
    {"lut_strength", "LUT strength", "شدة جدول الألوان"},
    {"bloom", "Bloom", "الوهج"},
    {"bloom_threshold", "Threshold", "العتبة"},
    {"bloom_knee", "Soft knee", "الانحناء الناعم"},
    {"bloom_intensity", "Bloom intensity", "شدة الوهج"},
    {"bloom_radius", "Bloom radius", "نصف قطر الوهج"},
    {"color_grade", "Color grading", "تصحيح الألوان"},
    {"contrast", "Contrast", "التباين"},
    {"pivot", "Pivot", "نقطة الارتكاز"},
    {"temperature", "Temperature", "حرارة اللون"},
    {"tint", "Tint", "الصبغة"},
    {"gamma", "Gamma", "غاما"},
    {"sharpen", "Sharpen", "الحدّة"},
    {"sharpen_amount", "Sharpen amount", "مقدار الحدّة"},
    {"sharpen_radius", "Sharpen radius", "نصف قطر الحدّة"},
    {"saturation", "Saturation", "التشبع"},
    {"vignette", "Vignette", "التظليل الجانبي"},
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
