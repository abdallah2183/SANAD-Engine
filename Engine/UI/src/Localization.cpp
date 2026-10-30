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

#include <atomic>
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

// The table is ONE flat array, not sorted: the keys are grouped by the panel
// or feature that introduced them (and later blocks are appended, not
// merged), so "keep sorted" is not the contract this file follows. The rows
// are also not free of dead weight — a number of keys are not referenced by
// the engine or the editor yet, and are kept on purpose so a half-built
// panel keeps its strings. Lookup is first-match-wins over a hash map built
// once, so the order of the rows does not change what a key means; only a
// DUPLICATE key would, and the table carries none.
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
    {"duplicate", "Duplicate", "تكرار"},
    {"direction", "Direction", "الاتجاه"},
    {"directional_light", "Directional Light", "الإضاءة الاتجاهية"},
    {"enabled", "Enabled", "مفعّل"},
    // A3: the outliner's empty-scene hint was a raw English literal.
    {"empty_scene_hint",
     "Empty scene — press + Empty to create your first entity, or drag a mesh from the "
     "Asset Browser into the viewport.",
     "مشهد فارغ — اضغط + فراغ لإنشاء أول كيان، أو اسحب مجسمًا من متصفح الأصول إلى منفذ العرض۔"},
    {"sky", "Sky", "السماء"},
    {"exposure", "Exposure", "التعريض"},
    {"far", "Far", "البعيد"},
    {"distance_fog", "Distance Fog", "ضباب المسافة"},
    {"fog_color", "Fog Color", "لون الضباب"},
    {"fog_start", "Fog Start", "بداية الضباب"},
    {"fog_end", "Fog End", "نهاية الضباب"},
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
    {"no_day_night", "No day/night cycle on this entity.", "لا توجد دورة ليل ونهار لهذا الكيان۔"},
    {"no_save_slots", "no save slots", "لا توجد خانات حفظ"},
    {"no_sky_settings", "No sky settings on this entity.", "لا توجد إعدادات سماء لهذا الكيان۔"},
    {"no_such_slot", "(no such slot)", "(لا توجد هذه الخانة)"},
    {"nothing_selected", "Nothing selected.", "لا يوجد تحديد۔"},
    {"open", "Open", "فتح"},
    {"outliner", "Outliner", "شجرة المشهد"},
    {"play", "Play", "تشغيل"},
    {"position", "Position", "الموضع"},
    {"prefab", "Prefab", "قالب"},
    {"redo", "Redo", "إعادة"},
    {"recover_draft", "Recover Last Draft", "استعادة آخر مسودة"},
    {"rigid_body", "Rigid Body", "الجسم الصلب"},
    {"rotation", "Rotation", "الدوران"},
    {"save", "Save", "حفظ"},
    {"save_as", "Save As...", "حفظ باسم..۔"},
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
    // First-run guide (Panels.cpp draw_start_here) and the viewport stats toggle.
    {"start_here_title", "Start here — your first game", "ابدأ من هنا — أول لعبة لك"},
    {"start_here_intro",
     "Three steps take you from an open editor to a running game. Reopen this window from "
     "Help > Start Here.",
     "ثلاث خطوات تنقلك من محرر مفتوح إلى لعبة تعمل۔ يمكنك فتح هذه النافذة من مساعدة > ابدأ من هنا۔"},
    {"start_here_step1", "1 — New scene (ground, sun, camera)",
                         "الخطوة الأولى: مشهد جديد (أرضية وشمس وكاميرا)"},
    {"start_here_step2", "2 — Add a ground plane", "الخطوة الثانية: إضافة أرضية اللعب"},
    {"start_here_step3", "3 — Play the game", "الخطوة الثالثة: تشغيل اللعبة وتجربتها"},
    {"start_here_tip",
     "Right-drag to look around, the wheel to zoom, W/E/R to move, rotate and scale. Drag a "
     "mesh from Assets into the viewport to place it.",
     "اسحب بالزر الأيمن للنظر حول المشهد، وبعجلة الفأرة للتقريب، وبمفاتيح W/E/R للتحريك "
     "والتدوير والتحجيم۔ اسحب مجسمًا من الأصول إلى منفذ العرض لوضعه۔"},
    {"start_here_menu", "Start Here", "ابدأ من هنا"},
    {"viewport_stats", "Viewport stats", "إحصاءات العرض"},
    {"assets_count", "assets", "أصل"},
    {"autosave_interval", "Autosave interval (s)", "فاصل الحفظ التلقائي (ث)"},
    {"browse", "Browse...", "استعراض..۔"},
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
    {"import_model", "Import model...", "استيراد مجسم..۔"},
    {"import_source", "Import source", "مصدر الاستيراد"},
    {"local_space", "Local", "محلي"},
    {"move", "Move", "تحريك"},
    {"new_scene", "New Scene", "مشهد جديد"},
    {"no_grid", "No grid", "بلا شبكة"},
    {"open_scene", "Open Scene...", "فتح مشهد..۔"},
    {"output_path", "Output file", "ملف الإخراج"},
    {"preferences", "Preferences...", "التفضيلات..۔"},
    {"rotate", "Rotate", "تدوير"},
    {"save_prefab", "Save as prefab", "حفظ كقالب"},
    {"save_scene", "Save Scene", "حفظ المشهد"},
    {"save_scene_as", "Save Scene As...", "حفظ المشهد باسم..۔"},
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
                           "اسحب التحديد لنقله (W/E/R لتغيير الأداة، Esc للإلغاء)۔"},
    {"viewport_hint_multi", "%zu selected - drag moves them all (one undo step).",
                            "المحدد: %zu - السحب ينقلها كلها (خطوة تراجع واحدة)۔"},
    {"viewport_hint_none", "No selection - gizmo disabled. Click the scene or an outliner row.",
                           "لا يوجد تحديد - الأداة معطلة. انقر في المشهد أو في شجرة المشهد۔"},
    {"viewport_hint_dragging", "Dragging %zu entr%s - release to commit, Esc cancels.",
                               "جارٍ سحب %zu كيان - أفلت للتثبيت، Esc للإلغاء۔"},
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
                      "التنقل: السحب الأيمن يدوّر، العجلة تقرّب، الاستمرار الأيمن مع WASD/QE للطيران۔"},
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
    {"no_reverb_zone", "No reverb zone on this entity.", "لا توجد منطقة صدى على هذا الكيان۔"},
    {"add_reverb_zone", "Add Reverb Zone", "إضافة منطقة صدى"},
    {"inner_radius", "Inner Radius", "نصف القطر الداخلي"},
    {"wet_gain", "Wet Gain", "مستوى البلل"},
    {"decay", "Decay (s)", "التلاشي (ث)"},
    {"predelay", "Pre-delay (s)", "التأخير المسبق (ث)"},
    {"echo_spacing", "Echo Spacing (s)", "تباعد الصدى (ث)"},
    {"music", "Music", "الموسيقى"},
    {"no_music", "No music on this entity.", "لا توجد موسيقى على هذا الكيان۔"},
    {"add_music", "Add Music", "إضافة موسيقى"},
    {"music_buffer", "Music Buffer", "مخزن الموسيقى"},
    {"ambience", "Ambience", "الجو المحيط"},
    {"no_ambience", "No ambience on this entity.", "لا توجد خلفية جو على هذا الكيان۔"},
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
    {"new_project", "New Project...", "مشروع جديد..۔"},
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
    {"ambience_volume", "Ambience Volume", "مستوى الأجواء"},
    {"voice_volume", "Voice Volume", "مستوى الحوار"},
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
    {"no_light_settings", "No light in this scene.", "لا توجد إضاءة في هذا المشهد۔"},
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
    {"sh_projects_sub", "Open a project or start a new one.", "افتح مشروعًا أو ابدأ مشروعًا جديدًا۔"},
    {"sh_new_title", "Create New Project", "إنشاء مشروع جديد"},
    {"sh_learn_title", "Learning Sanad", "تعلّم سند"},
    {"sh_help_title", "Sanad Help", "مساعدة سند"},
    {"sh_settings_title", "Sanad Settings", "إعدادات سند"},
    {"sh_store_title", "Sanad Asset Store", "متجر أصول سند"},
    {"sh_store_sub", "Curated packages — opening soon.", "حزم مختارة — الافتتاح قريبًا۔"},
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
    {"sh_no_match", "No projects match this search.", "لا توجد مشاريع تطابق هذا البحث۔"},
    {"sh_soon", "soon", "قريبًا"},
    {"sh_tpl_nature", "Nature Exploration", "استكشاف الطبيعة"},
    {"sh_tpl_platformer", "Core Platformer", "منصات أساسي"},
    {"sh_tpl_arena", "FPS Arena", "ساحة تصويب"},
    // The card names must match the folder they scaffold: sh_tpl_side points at
    // the ThirdPerson template (a 3D character), not a 2D side-scroller, and
    // sh_tpl_blank points at Default, which already ships a ground and a cube.
    {"sh_tpl_side", "Third-Person Starter", "بداية منظور ثالث"},
    {"sh_tpl_blank", "Starter Scene", "مشهد جاهز"},
    {"sh_tpl_marine", "Marine Environment", "بيئة بحرية"},
    {"sh_setup", "Project Setup", "إعداد المشروع"},
    {"sh_dim_all", "All (2D / 3D)", "الكل (ثنائي وثلاثي الأبعاد)"},
    {"sh_dim_2d", "2D Games", "ألعاب ثنائية الأبعاد"},
    {"sh_dim_3d", "3D Games", "ألعاب ثلاثية الأبعاد"},
    {"sh_dim_label", "Game Type:", "نوع اللعبة:"},
    {"sh_dim_opt_all", "All / Hybrid", "الكل / هجين"},
    {"sh_dim_opt_2d", "2D Game", "لعبة ثنائية الأبعاد"},
    {"sh_dim_opt_3d", "3D Game", "لعبة ثلاثية الأبعاد"},
    {"view_mode_2d", "2D Mode (Orthographic)", "النمط ثنائي الأبعاد"},
    {"view_mode_3d", "3D Mode (Perspective)", "النمط ثلاثي الأبعاد"},
    {"sh_name_label", "Project Name:", "اسم المشروع:"},
    {"sh_loc_label", "Project Location:", "موقع المشروع:"},
    {"sh_renderer_label", "Target Renderer:", "العارض المستهدف:"},
    {"sh_renderer_value", "Vulkan (fixed)",
                          "فولكان (ثابت)"},
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
                             "دورات منظمة ودرس تعريفي في 20 دقيقة۔"},
    {"sh_learn_script", "Visual Scripting Guide", "دليل البرمجة المرئية"},
    {"sh_learn_script_sub", "Node scripting — coming with the visual system.",
                            "برمجة العقد — قادمة مع النظام المرئي۔"},
    {"sh_learn_pipeline", "Asset Pipeline", "خط إنتاج الأصول"},
    {"sh_learn_pipeline_sub", "Importing and managing 2D/3D content.",
                              "استيراد وإدارة المحتوى ثنائي وثلاثي الأبعاد۔"},
    {"sh_learn_api", "SanadScript API", "واجهة سند سكربت"},
    {"sh_learn_api_sub", "Full reference for Lua and C# gameplay.",
                         "مرجع كامل للعب بلغة لوا وسي شارب۔"},
    {"sh_learn_physics", "Physics & Collision", "الفيزياء والتصادم"},
    {"sh_learn_physics_sub", "Jolt bodies, characters, vehicles, destruction.",
                             "أجسام جولت والشخصيات والمركبات والتدمير۔"},
    {"sh_learn_gfx", "Advanced Graphics", "الرسوميات المتقدمة"},
    {"sh_learn_gfx_sub", "PBR, shadows, and the render graph.", "التظليل الفيزيائي والظلال ومخطط العرض۔"},
    {"sh_help_docs", "Documentation", "التوثيق"},
    {"sh_help_docs_sub", "Complete reference in the Docs folder.", "مرجع كامل في مجلد المستندات۔"},
    {"sh_help_tuts", "Tutorials & Guides", "الدروس والأدلة"},
    {"sh_help_tuts_sub", "Step-by-step guides in Docs.", "أدلة خطوة بخطوة في المستندات۔"},
    {"sh_help_forum", "Community Forum", "منتدى المجتمع"},
    {"sh_help_forum_sub", "Meet other developers — online soon.", "قابل مطورين آخرين — قريبًا عبر الإنترنت۔"},
    {"sh_help_notes", "Release Notes", "ملاحظات الإصدار"},
    {"sh_help_notes_sub", "What changed (ROADMAP.md).", "ما الجديد (ROADMAP.md)۔"},
    {"sh_help_support", "Support Tickets", "تذاكر الدعم"},
    {"sh_help_support_sub", "Formal support channel — online soon.", "قناة دعم رسمية — قريبًا عبر الإنترنت۔"},
    {"sh_help_feature", "Feature Requests", "طلبات الميزات"},
    {"sh_help_feature_sub", "Vote on ideas — online soon.", "صوّت على الأفكار — قريبًا عبر الإنترنت۔"},
    // Status-line messages. The trailing-colon rows are PREFIXES: the path is
    // appended after a newline, so the bidi run of the Latin path stays intact
    // (shape_arabic keeps Latin runs in order inside an Arabic line).
    {"sh_st_missing", "That project file no longer exists:", "ملف المشروع لم يعد موجودًا:"},
    {"sh_st_choose_loc", "Choose a location for the project.", "اختر موقعًا للمشروع۔"},
    {"sh_st_exists", "A folder with that name already exists:", "يوجد مجلد بهذا الاسم بالفعل:"},
    {"sh_st_create_fail", "Could not create the project:", "تعذر إنشاء المشروع:"},
    {"sh_st_low_integrity",
     "Windows runs this copy at Low integrity (downloaded ZIP), so Desktop and Documents are "
     "read-only. Run FixPermissions.cmd once, then reopen the editor.",
     "يشغّل ويندوز هذه النسخة بصلاحية منخفضة لأنها مستخرجة من أرشيف مُنزَّل، لذا سطح المكتب "
     "والمستندات للقراءة فقط۔ شغّل ملف إصلاح الصلاحيات المرفق مرة واحدة ثم أعد فتح المحرر۔"},
    {"sh_st_low_integrity_hint",
     "Run FixPermissions.cmd in the engine folder once, then reopen the editor.",
     "شغّل ملف إصلاح الصلاحيات المرفق مع المحرك مرة واحدة ثم أعد فتح المحرر۔"},
    {"sh_st_low_integrity_dir", "New projects go to:", "تُنشأ المشاريع الجديدة في:"},
    {"sh_st_nfproj_missing", "The project was created but its .nfproj is missing:",
                             "أُنشئ المشروع لكن ملف .nfproj مفقود:"},
    {"sh_st_docs_missing", "Docs folder not found beside this build.",
                           "لم يُعثر على مجلد المستندات بجانب هذا البناء۔"},
    {"sh_st_script_soon", "Visual scripting arrives with the node system.",
                          "البرمجة المرئية قادمة مع نظام العقد۔"},
    {"sh_st_roadmap_missing", "ROADMAP.md not found beside this build.",
                              "لم يُعثر على ROADMAP.md بجانب هذا البناء۔"},
    {"sh_st_community_soon", "Online community opens with the Asset Store.",
                             "يُفتتح المجتمع عبر الإنترنت مع متجر الأصول۔"},
    {"sh_st_cleared", "Recent list cleared.", "مُسحت قائمة المشاريع الأخيرة۔"},
    // --- Shell upgrade: empty library, template blurbs, confirm/drop/remove --
    {"sh_empty_title", "No projects yet.", "لا توجد مشاريع بعد۔"},
    {"sh_empty_hint", "Create your first project to get started.", "أنشئ مشروعك الأول للبدء۔"},
    {"sh_tpl_nature_desc", "Open-world foliage and terrain sample.", "عينة عالم مفتوح بعشب وتضاريس۔"},
    {"sh_tpl_platformer_desc", "Side-view jumping and platforms.", "قفز ومنصات بعرض جانبي۔"},
    {"sh_tpl_arena_desc", "First-person arena combat.", "قتال ساحة بمنظور أول۔"},
    {"sh_tpl_side_desc", "A 3D character you walk around a level.",
                          "شخصية ثلاثية الأبعاد تتجول بها في مستوى۔"},
    {"sh_tpl_blank_desc", "Ground, cube and sun — a room to build in.",
                           "أرضية ومكعب وشمس — مساحة تبني فيها۔"},
    {"sh_tpl_marine_desc", "Underwater world and vehicles.", "عالم تحت الماء ومركبات۔"},
    {"sh_confirm_clear_title", "Clear recent list", "مسح القائمة الأخيرة"},
    {"sh_confirm_clear_text", "Forget all recent projects? This cannot be undone.",
                              "نسيان كل المشاريع الأخيرة؟ لا يمكن التراجع۔"},
    {"sh_st_drop_only", "Drop a single .nfproj file to open it.", "أفلت ملف .nfproj واحد لفتحه۔"},
    {"sh_st_removed", "Removed from the recent list.", "أُزيل من قائمة المشاريع الأخيرة۔"},
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
    {"err_name_empty", "Enter a project name.", "أدخل اسم المشروع۔"},
    {"err_name_long", "Name is too long (64 characters max).", "الاسم طويل جدًا (64 حرفًا كحد أقصى)۔"},
    {"err_name_fragment", "That is a path fragment, not a name.", "هذا جزء مسار وليس اسمًا۔"},
    {"err_name_edges", "Name cannot start or end with a space or a dot.",
                       "لا يمكن أن يبدأ الاسم أو ينتهي بمسافة أو نقطة۔"},
    {"err_name_forbidden", "Name contains a character Windows forbids in a path.",
                           "يحتوي الاسم على حرف يمنعه ويندوز في المسارات۔"},
    {"err_name_control", "Name contains a control character.", "يحتوي الاسم على حرف تحكم۔"},
    {"err_name_reserved", "Name is a reserved Windows device name.", "الاسم محجوز كاسم جهاز في ويندوز۔"},

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
    {"no_destructible", "No destructible on this entity.", "لا يوجد جسم قابل للتدمير لهذا الكيان۔"},
    {"no_rigid_body", "No rigid body on this entity.", "لا يوجد جسم صلب لهذا الكيان۔"},
    {"no_collider", "No collider on this entity.", "لا يوجد متصادم لهذا الكيان۔"},
    {"chunks", "Chunks", "الشظايا"},
    {"seed", "Seed", "البذرة"},
    {"strength", "Strength", "القوة"},
    {"damage_threshold", "Damage threshold", "عتبة الضرر"},
    {"blast_radius", "Blast radius", "نصف قطر الانفجار"},
    {"buffer", "Buffer", "المخزن"},
    {"attach_buffer", "Attach buffer", "إرفاق مخزن"},
    {"particles", "Particles", "الجسيمات"},
    {"no_particles", "No particle emitter on this entity.", "لا يوجد باعث جسيمات لهذا الكيان۔"},
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
    {"no_cloth", "No cloth on this entity.", "لا يوجد قماش لهذا الكيان۔"},
    {"res_x", "Res X", "الدقة الأفقية"},
    {"res_z", "Res Z", "الدقة العمقية"},
    {"spacing", "Spacing", "التباعد"},
    {"damping", "Damping", "التخميد"},
    {"stiffness", "Stiffness", "الصلابة"},
    {"iterations", "Iterations", "التكرارات"},
    {"substeps", "Substeps", "الخطوات الفرعية"},
    {"character", "Character", "الشخصية"},
    {"no_character", "No character on this entity.", "لا توجد شخصية لهذا الكيان۔"},
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
     "لا يوجد حجم تنقل لهذا الكيان۔"},
    {"nav_agent", "Nav Agent", "عميل التنقل"},
    {"no_nav_agent", "No nav agent on this entity.", "لا يوجد عميل تنقل لهذا الكيان۔"},
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
    {"nav_links", "Authored links", "الروابط المؤلَّفة"},
    {"terrain", "Terrain", "التضاريس"},
    {"no_terrain", "No terrain on this entity.", "لا توجد تضاريس لهذا الكيان۔"},
    {"terrain_res", "Resolution", "الدقة"},
    {"terrain_area", "Area (metres)", "المساحة (متر)"},
    {"terrain_height", "Height scale", "مقياس الارتفاع"},
    {"terrain_freq", "Noise frequency", "تردد الضجيج"},
    {"terrain_octaves", "Octaves", "الطبقات"},
    {"terrain_seed", "Seed", "البذرة"},
    {"terrain_heightmap", "Heightmap", "خريطة الارتفاع"},
    {"terrain_material", "Material", "المادة"},
    {"goal", "Goal", "الهدف"},
    {"arrive_radius", "Arrive radius", "نصف قطر الوصول"},
    // Phase 64: authored patrol routes. The live cursor has no editor of its
    // own — these strings label the authored list and the read-only readout.
    {"nav_route", "Patrol Route", "مسار الدورية"},
    {"no_nav_route", "No patrol route on this entity.",
     "لا يوجد مسار دورية لهذا الكيان."},
    {"route_loop", "Loop", "تكرار"},
    {"route_ping_pong", "Ping-pong", "ذهاب وعودة"},
    {"route_wait", "Wait (s)", "انتظار (ث)"},
    // Phase 65: the perception gate and its post-sighting delay.
    {"route_hold", "Hold patrol", "تعليق الدورية"},
    {"route_hold_off", "Off", "معطّل"},
    {"route_hold_aware", "While aware", "عند المعرفة"},
    {"route_hold_sensing", "While sensing", "عند الإحساس"},
    {"route_hold_hint",
     "Hold the patrol while the walker's senses know a hostile target. A walker "
     "with no team treats everyone it notices as a threat.",
     "تتوقف الدورية ما دام إدراك العميل يعرف هدفًا معاديًا. والعميل بلا فريق يعدّ "
     "كل من يلاحظه تهديدًا."},
    {"route_resume", "Resume after (s)", "استئناف بعد (ث)"},
    {"route_resume_hint", "Seconds to keep holding after the last sighting.",
     "ثواني الاستمرار في التعليق بعد آخر إحساس بالهدف."},
    // Phase 66: the search of the last known position once the hold ends.
    {"route_investigate", "Investigate (s)", "تحقيق (ث)"},
    {"route_investigate_hint",
     "Seconds to search the last known position after the hold ends.",
     "ثواني البحث في آخر موضع معروف بعد انتهاء التعليق."},
    {"route_investigating", "Investigating the last known position.",
     "تحقق في آخر موضع معروف."},
    // Phase 67: the squad that answers the same call.
    {"route_squad", "Squad", "الفصيل"},
    {"route_squad_hint",
     "Shared search: walkers with the same token answer each other's "
     "investigation. Leave empty for a solo search.",
     "بحث مشترك: العملاء الذين يحملون الرمز نفسه يستجيبون لتحقيق بعضهم. اتركه "
     "فارغًا لبحث فردي."},
    {"route_held", "Held by perception.", "معلّقة بسبب الإدراك."},
    {"route_points", "Destinations", "المحطات"},
    {"route_point", "Point", "نقطة"},
    {"route_add_point", "Add point", "إضافة نقطة"},
    {"route_point_wait_hint",
     "Seconds to wait at this point; -1 uses the route wait.",
     "ثواني الانتظار عند هذه النقطة؛ ‎-1 يستخدم انتظار المسار."},
    {"route_live", "Live route", "المسار الجاري"},
    {"route_finished", "Patrol finished at its last point.",
     "انتهت الدورية عند آخر نقطة."},
    // Step 11: the drivable car. Tune keys mirror the Vehicle: scene line;
    // the four inputs are the debug-drive sliders (live only, never saved).
    {"vehicle", "Vehicle", "مركبة"},
    {"no_vehicle", "No vehicle on this entity.", "لا توجد مركبة لهذا الكيان۔"},
    {"chassis", "Chassis half extents", "أنصاف أبعاد الهيكل"},
    {"wheel_radius", "Wheel radius", "نصف قطر العجلة"},
    {"wheel_width", "Wheel width", "عرض العجلة"},
    {"track_width", "Track half width", "نصف عرض المسار"},
    {"wheelbase", "Wheelbase half length", "نصف طول قاعدة العجلات"},
    {"wheel_height", "Wheel attach height", "ارتفاع تثبيت العجلة"},
    {"steer_angle", "Max steer (deg)", "أقصى توجيه (درجة)"},
    {"engine_torque", "Engine torque", "عزم المحرك"},
    {"susp_min", "Suspension min travel", "أدنى مشوار التعليق"},
    {"susp_max", "Suspension max travel", "أقصى مشوار التعليق"},
    {"susp_freq", "Suspension frequency", "تردد التعليق"},
    {"tire_peak", "Tire peak friction", "ذروة احتكاك الإطار"},
    {"tire_limit", "Tire limit friction", "حد احتكاك الإطار"},
    {"wheel_inertia", "Wheel inertia", "عطالة العجلة"},
    {"brake_torque", "Brake torque", "عزم الفرامل"},
    {"handbrake_torque", "Handbrake torque", "عزم فرامل اليد"},
    {"throttle", "Throttle", "الوقود"},
    {"steer", "Steer", "التوجيه"},
    {"brake", "Brake", "الفرامل"},
    {"handbrake", "Handbrake", "فرامل اليد"},
    {"reset_vehicle", "Reset vehicle", "إعادة ضبط المركبة"},
    // Step 8: the viewport debug overlay toggle (View menu) and its stats
    // line. The overlay draws the baked mesh plus every agent's remaining
    // route; the stats line is the "is anything navigating" glance.
    {"show_navmesh", "Navigation debug", "تنقيح التنقل"},
    {"stats_navmesh", "%zu nav polys (%zu/%zu agents at goal)", "%zu مضلع تنقل (%zu/%zu عميل عند الهدف)"},
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
    {"stats_vehicles", "%zu vehicles", "%zu مركبات"},
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
     "مجلد فارغ — اسحب المجسمات هنا أو أنشئ سكربت۔"},
    {"no_files_match", "No files match the filter.", "لا توجد ملفات تطابق التصفية۔"},
    {"folders_count_fmt", "%zu folders", "%zu مجلد"},
    {"files_count_fmt", "%zu files", "%zu ملف"},
    {"create_menu", "New...", "جديد..۔"},
    {"collapse_all", "Collapse", "طي"},
    // --- Asset delete / rename (FileSystem dock context menus) --------------
    {"delete_asset", "Delete", "حذف الأصل"},
    {"delete_folder", "Delete folder", "حذف المجلد"},
    {"rename_asset", "Rename...", "إعادة تسمية..۔"},
    {"delete_confirm_title", "Delete", "حذف"},
    {"delete_confirm", "Delete '%s'?", "حذف «%s»؟"},
    {"delete_confirm_folder",
     "Delete the folder '%s' and everything inside it? This cannot be undone.",
     "حذف المجلد «%s» وكل ما بداخله؟ لا يمكن التراجع۔"},
    {"rename_prompt", "New name", "الاسم الجديد"},
    // A failed rename used to fall through tr()'s unknown-key path, which
    // returns the KEY ITSELF — the user saw the literal text "rename_failed"
    // in the console, in both languages.
    {"rename_failed", "Rename failed", "فشل إعادة التسمية"},
    {"name_no_spaces", "Name cannot contain spaces.", "الاسم لا يمكن أن يحتوي مسافات۔"},
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
     "(جميع النصوص العربية غير مقروءة حالياً)۔"},
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
                       "تغييرات غير محفوظة — اضغط Ctrl+S للحفظ۔"},
    {"status_ok", "No validation errors", "لا أخطاء تحقق"},
    {"status_errors", "%u validation errors", "%u أخطاء تحقق"},
    {"untitled", "Untitled", "بلا عنوان"},
    {"axis_gizmo_hint", "World axes — the current view orientation.",
                        "محاور العالم — اتجاه العرض الحالي۔"},
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
    {"follow", "Follow", "تتبع"},
    {"follow_target", "Follow target", "هدف التتبع"},
    {"follow_hint", "The name of the entity the camera follows. Empty means it follows nothing.",
     "اسم المسمى الكان التتبعه. فارغ يمنع أنه يتتبع شيئا."},
    {"follow_pivot", "Pivot", "نقطة الارتكاز"},
    {"follow_distance", "Distance", "المسافة"},
    {"follow_yaw", "Yaw", "الدوران الأفقي"},
    {"follow_pitch", "Pitch", "الميل"},
    {"follow_smoothing", "Smoothing", "النعومة"},
    {"follow_collision", "Collision radius", "نصف التصادم"},
    {"follow_avoid", "Avoid obstacles", "تجنّب العوائق"},
    {"aim", "Aiming", "التوجيه"},
    {"aim_mouse", "Mouse look", "التوجيه بالفأرة"},
    {"aim_hint", "While playing, the mouse turns the camera and the wheel changes the distance. Esc gives the pointer back.",
     "أثناء اللعب، الفأرة تدوّر الكاميرا والعجلة تغيّر المسافة. زر Esc يرجّع المؤشر."},
    {"aim_sensitivity", "Mouse sensitivity", "حساسية الفأرة"},
    {"aim_stick", "Stick speed", "سرعة العصا"},
    {"aim_pitch_min", "Lowest pitch", "أدنى ميل"},
    {"aim_pitch_max", "Highest pitch", "أعلى ميل"},
    {"aim_invert_y", "Invert vertical", "عكس المحور العمودي"},
    {"aim_zoom_min", "Closest zoom", "أقرب تقريب"},
    {"aim_zoom_max", "Farthest zoom", "أبعد تقريب"},
    {"aim_zoom_step", "Zoom step", "خطوة التقريب"},
    {"aim_align", "Auto-align", "محاذاة تلقائية"},
    {"aim_ads", "Aim down sights", "التصويب المزدوج"},
    {"aim_ads_distance", "Sights distance", "مسافة التصويب"},
    {"aim_ads_sensitivity", "Sights sensitivity", "حساسية التصويب"},
    {"aim_ads_speed", "Sights speed", "سرعة التصويب"},
    {"aim_shake", "Camera shake", "اهتزاز الكاميرا"},
    {"aim_shake_strength", "Shake strength", "قوة الاهتزاز"},
    {"aim_shake_decay", "Shake decay", "تلاشي الاهتزاز"},
    {"aim_recoil", "Recoil recovery", "تلاشي الارتداد"},
    {"aim_recoil_max", "Recoil limit", "حد الارتداد"},
    {"player_settings", "Player settings", "إعدادات اللاعب"},
    {"settings_file", "File", "الملف"},
    {"look_scale", "Mouse scale", "مضاعف الفأرة"},
    {"stick_scale", "Stick scale", "مضاعف العصا"},
    {"invert_y", "Invert Y", "قلب المحور الرأسي"},
    {"invert_auto", "Auto (scene)", "تلقائي (المشهد)"},
    {"invert_on", "Inverted", "مقلوب"},
    {"invert_off", "Normal", "عادي"},
    {"ads_scale", "ADS scale", "مضاعف التصويب"},
    {"shake_scale", "Shake scale", "مضاعف الاهتزاز"},
    {"audio_dynamics", "Audio dynamics", "ديناميكيات الصوت"},
    {"compressor", "Compressor", "ضاغط الصوت"},
    {"limiter", "Peak limiter", "محدّد الذروة"},
    {"dyn_threshold", "Threshold", "عتبة الضغط"},
    {"dyn_ratio", "Ratio", "نسبة الضغط"},
    {"dyn_attack", "Attack", "سرعة الهجوم"},
    {"dyn_release", "Release", "سرعة التحرير"},
    {"dyn_knee", "Knee", "نعومة العتبة"},
    {"dyn_makeup", "Makeup gain", "تعويض الصوت"},
    {"dyn_ceiling", "Ceiling", "سقف الذروة"},
    {"dyn_lookahead", "Lookahead", "الاستباق"},
    {"reload", "Reload", "إعادة قراءة"},
    {"input_bindings", "Input bindings", "ربط الأزرار"},
    {"binding_conflicts", "Conflicts", "تعارضات"},
    {"rebind", "Rebind", "إعادة ربط"},
    {"reset", "Reset", "إعادة ضبط"},
    {"press_a_key", "Press a key…", "اضغط زرًا…"},
    {"bindings_hint", "Rebind, then press a key or a mouse button. Esc cancels.",
     "اضغط «إعادة ربط» ثم اضغط زرًا أو زر فأرة. Esc يلغي."},
    {"frame_policy", "Decide from the frame window", "القرار من نافذة الإطارات"},
    {"frame_policy_hint", "Uses the 1% low and the average instead of the last frame. Needs a host that feeds the window (the player does).",
     "يستخدم أدنى ١٪ والمتوسط بدل الإطار الأخير. يحتاج مضيفًا يغذّي النافذة (اللاعب يفعل)."},
    {"profiles", "Player profiles", "ملفات اللاعبين"},
    {"profile_name", "Profile name", "اسم الملف"},
    {"save_profile", "Save as profile", "حفظ كملف"},
    {"load_profile", "Load profile", "تحميل ملف"},
    {"profile_hint", "Switching copies the profile over the active settings, so the game reads it too.",
     "التبديل ينسخ الملف فوق الإعدادات النشطة، فتقرأه اللعبة أيضًا."},
    {"performance", "Performance", "الأداء"},
    {"statistics", "Statistics", "الإحصاءات"},
    {"frame_time", "Frame", "الإطار"},
    {"gpu_time", "GPU", "المعالج الرسومي"},
    {"gpu_avg", "GPU (avg)", "المعالج الرسومي (متوسط)"},
    {"gpu_timestamp", "GPU (timestamp)", "المعالج الرسومي (طابع وقت)"},
    {"gpu_passes", "GPU passes", "مراحل المعالج الرسومي"},
    {"gpu_timing_note", "Timestamps include profiling drains; sampled one frame late.",
     "الطوابع الزمنية تشمل تصريف خط الأنابيب بسبب القياس، وتُقرأ بعد إطار واحد."},
    {"gpu_timing_unavailable", "GPU timestamps unavailable; showing submit-to-fence estimate.",
     "طوابع وقت المعالج الرسومي غير متاحة؛ يُعرض تقدير الإرسال حتى السياج."},
    {"fps_avg", "Average FPS", "متوسط الإطارات/ث"},
    {"fps_1pct_low", "1% low FPS", "أدنى ١٪ (إطار/ث)"},
    {"frame_jitter", "Frame jitter", "تفاوت الإطارات"},
    {"frame_bound", "Bottleneck", "عنق الزجاجة"},
    {"bound_cpu", "CPU", "المعالج"},
    {"bound_gpu", "GPU", "المعالج الرسومي"},
    {"bound_balanced", "Balanced", "متوازن"},
    {"bound_unknown", "Unknown", "غير معروف"},
    {"frame_pacing_hint",
     "1% low is the mean of the slowest 1% of frames; jitter is the standard deviation of "
     "those frame times. Presentation/vsync waiting counts toward CPU.",
     "أدنى ١٪ هو متوسط أبطأ ١٪ من الإطارات؛ والتفاوت هو الانحراف المعياري. انتظار العرض "
     "والتزامن الرأسي يُحتسب ضمن جانب المعالج."},
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
     "لا توجد إضاءة اتجاهية في هذا المشهد — يستخدم العارض الإعداد الافتراضي. أضف واحدة لتحرير الإضاءة۔"},
    {"environment_none_hint", "No sky in this scene — the renderer paints its cold-start sky. Add one to author the environment.",
     "لا توجد سماء في هذا المشهد — يرسم العارض سماء البداية. أضف واحدة لتحرير البيئة۔"},
    {"camera_none_hint", "No camera in this scene. Add one so the game has a viewpoint to render from.",
     "لا توجد كاميرا في هذا المشهد. أضف واحدة ليكون للعبة منظور تُعرض منه۔"},
    {"shadow_distance_hint", "0 = use the camera's far plane. Lower values concentrate the shadow atlas on the range a player can read.",
     "٠ = استخدام المستوى البعيد للكاميرا. القيم الأقل تركّز خرائط الظل على المدى الذي يراه اللاعب۔"},
    {"time_of_day_hint", "Blends the three sky presets through a full day. Drag to preview live, then Apply.",
     "يمزج أنماط السماء الثلاثة عبر يوم كامل. اسحب للمعاينة المباشرة ثم طبّق۔"},
    {"camera_transform_hint", "Position and rotation come from the entity's Transform, which is also what the viewport camera follows.",
     "الموضع والدوران من تحويل الكيان، وهو ما تتبعه كاميرا منفذ العرض أيضًا۔"},
    // --- Post-processing stack (design §206) --------------------------------
    {"post_process", "Post-processing", "المعالجة اللاحقة"},
    {"no_post_process", "No post-processing on this entity. The renderer's defaults apply.",
     "لا توجد معالجة لاحقة لهذا الكيان. تُستخدم الإعدادات الافتراضية للعارض۔"},
    {"add_post_process", "Add post-processing", "إضافة معالجة لاحقة"},
    {"post_process_hint", "Every stage is off by default and an off stage costs nothing. Bloom records a real blur chain; the rest run inside the tonemap pass.",
     "كل مرحلة متوقفة افتراضيًا والمرحلة المتوقفة لا تكلف شيئًا. الوهج يشغّل سلسلة تمويه حقيقية، والبقية تعمل داخل تمريرة تعيين الدرجات۔"},
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
    {"anti_aliasing", "Anti-aliasing (FXAA)", "مضاد التعرّج (FXAA)"},
    {"edge_threshold", "Edge threshold", "عتبة الحافة"},
    {"edge_floor", "Edge floor", "الحد الأدنى للحافة"},
    // Internal render scale (Phase 37). Arabic values deliberately avoid Latin
    // text: the number is the value, and "supersampling" is translated rather
    // than kept as a brand-like token.
    {"render_scale", "Render scale", "مقياس الرسم"},
    {"supersampling", "Supersampling", "دقة فائقة"},
    {"render_scale_hint", "Above 1 renders larger and downsamples; below 1 renders fewer pixels and upscales.",
     "أعلى من واحد يرسم أكبر ثم يصغّر، وأدنى من واحد يرسم بكسلًا أقل ثم يكبّر۔"},
    // Dynamic resolution (Phase 38). The static scale is the ceiling, so the
    // hint names it instead of repeating the bounds, which live in code.
    {"dynamic_resolution", "Dynamic resolution", "دقة ديناميكية"},
    {"target_fps", "Target frame rate", "معدّل الإطارات المستهدف"},
    {"min_scale", "Minimum scale", "أدنى مقياس"},
    {"dynamic_resolution_hint",
     "Lowers the scale when frames run long and restores it when there is headroom. The render scale above is the ceiling.",
     "يخفض المقياس عندما يطول الإطار ويعيده عندما تتوفر فسحة. مقياس الرسم أعلاه هو السقف۔"},
    // Phase 40: the governor can read real GPU timestamps. This is a preference
    // with fallback, so the hint says so instead of promising GPU-only behavior.
    {"timing_source", "Timing source", "مصدر التوقيت"},
    {"timing_cpu", "CPU frame time", "زمن إطار المعالج"},
    {"timing_gpu", "GPU timestamps", "طوابع زمن المعالج الرسومي"},
    {"dynamic_resolution_timing_hint",
     "GPU uses real frame timestamps when available and falls back to CPU frame time until a fresh sample arrives.",
     "يستخدم المعالج الرسومي طوابع زمن الإطار عند توفرها، ويعود إلى زمن إطار المعالج عند غياب عيّنة جديدة۔"},
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
    // --- Projected decals (Phase 36) -----------------------------------------
    {"decal", "Decal", "ملصق"},
    {"no_decal", "No decal on this entity. Add one to project a material onto the surfaces behind it.",
     "لا يوجد ملصق على هذا الكيان. أضف واحدًا لإسقاط خامة على الأسطح خلفه۔"},
    {"add_decal", "Add decal", "إضافة ملصق"},
    {"decal_hint", "The entity's local +Z faces the surface, and its Transform places and rotates the box. Size is the full extent in metres.",
     "يتجه المحور الأمامي المحلي للكيان نحو السطح، ويحدد تحويله موضع الصندوق ودورانه. الحجم هو الامتداد الكامل بالأمتار۔"},
    {"decal_size", "Size (X, Y, depth)", "الحجم (العرض، الارتفاع، العمق)"},
    {"decal_opacity", "Opacity", "الشفافية"},
    // --- Weapons and hit points (Phase 44) -----------------------------------
    //
    // The panel's own words. "health" and "spread" already existed (the game
    // UI's bar and the material panel), so they are reused rather than
    // duplicated — a second key with the same label is how two screens start
    // drifting apart.
    {"weapon", "Weapon", "السلاح"},
    {"add_weapon", "Add weapon", "إضافة سلاح"},
    {"fire_mode", "Fire mode", "نمط الإطلاق"},
    {"fire_semi", "Semi", "فردي"},
    {"fire_auto", "Auto", "آلي"},
    {"fire_burst", "Burst", "رشقة"},
    {"rpm", "Rate of fire (rpm)", "معدل الإطلاق (طلقة/دقيقة)"},
    {"magazine", "Magazine", "المخزن"},
    {"reserve", "Reserve", "الاحتياط"},
    {"reload_time", "Reload time", "زمن التعبئة"},
    {"damage", "Damage", "الضرر"},
    {"range", "Range", "المدى"},
    {"pellets", "Pellets", "عدد الحبيبات"},
    {"spread_max", "Maximum spread", "أقصى تشتت"},
    {"recoil_pitch", "Vertical recoil", "الارتداد العمودي"},
    {"recoil_yaw", "Horizontal recoil", "الارتداد الأفقي"},
    {"auto_reload", "Reload when empty", "التعبئة عند الفراغ"},
    {"add_health", "Add hit points", "إضافة نقاط صحة"},
    {"max_health", "Maximum health", "أقصى صحة"},
    {"health_now", "Health", "الصحة"},
    {"regen", "Regeneration", "التجدد"},
    {"regen_delay", "Regeneration delay", "تأخير التجدد"},
    {"invulnerable", "Invulnerable", "محصّن"},
    // --- Layered damage (Phase 50) ------------------------------------------
    {"armor_penetration", "Armor penetration", "اختراق الدرع"},
    {"armor_max", "Maximum armor", "أقصى درع"},
    {"armor_now", "Armor", "الدرع"},
    {"armor_absorb", "Armor absorption", "امتصاص الدرع"},
    {"hit_zones", "Hit zones (head / limbs)", "مناطق الإصابة (رأس / أطراف)"},
    {"headshot_multiplier", "Headshot multiplier", "مضاعف إصابة الرأس"},
    {"limb_multiplier", "Limb multiplier", "مضاعف إصابة الأطراف"},
    // --- Radial blasts (Phase 51) -------------------------------------------
    // `blast_radius` already exists in the destruction panel; reuse it rather
    // than shipping a duplicate key with the same words.
    {"blast_damage", "Blast damage", "ضرر الانفجار"},
    {"blast_min", "Damage at the edge", "الضرر عند الحافة"},
    // --- Damage types and resistance (Phase 52) -----------------------------
    {"damage_type", "Damage type", "نوع الضرر"},
    {"damage_ballistic", "Ballistic", "رصاص"},
    {"damage_explosive", "Explosive", "انفجار"},
    {"damage_fire", "Fire", "نار"},
    {"resist_ballistic", "Ballistic resistance", "مقاومة الرصاص"},
    {"resist_explosive", "Explosive resistance", "مقاومة الانفجار"},
    {"resist_fire", "Fire resistance", "مقاومة النار"},
    // --- Status effects (Phase 54) ------------------------------------------
    {"status_effect", "Status on hit", "الحالة عند الإصابة"},
    {"status_none", "None", "بلا"},
    {"status_burn", "Burning", "حرق"},
    {"status_bleed", "Bleeding", "نزيف"},
    {"status_corrode", "Corrosion", "تآكل"},
    {"status_dps", "Status damage/s", "ضرر الحالة/ث"},
    {"status_time", "Status duration", "مدة الحالة"},
    {"status_tick", "Status tick", "نبضة الحالة"},
    // --- Combat awareness (Phase 55) ----------------------------------------
    {"retaliate", "Retaliate against last attacker", "الرد على آخر مهاجم"},
    {"suppression", "Suppression", "الكبت"},
    // --- Ballistics, factions and agent authoring (Phase 46) ----------------
    {"delivery", "Delivery", "طريقة الإطلاق"},
    {"delivery_hitscan", "Hitscan", "لحظي"},
    {"delivery_projectile", "Projectile", "مقذوف"},
    {"projectile_speed", "Muzzle speed", "سرعة الفوهة"},
    {"projectile_gravity", "Bullet gravity", "جاذبية الرصاصة"},
    {"combat_agent", "Combat agent", "وكيل قتال"},
    {"add_combat_agent", "Add combat agent", "إضافة وكيل قتال"},
    {"auto_target", "Choose enemy by faction", "اختيار العدو حسب الفصيل"},
    {"agent_eye", "Eye height", "ارتفاع العين"},
    {"agent_aim_height", "Aim height", "ارتفاع التصويب"},
    {"agent_reaction", "Reaction time", "زمن رد الفعل"},
    {"agent_turn", "Turn speed", "سرعة الاستدارة"},
    {"agent_cone", "Fire cone", "مخروط الإطلاق"},
    {"agent_range", "Engagement range", "مدى الاشتباك"},
    {"agent_burst", "Burst duration", "مدة الرشقة"},
    {"agent_rest", "Rest between bursts", "سكون بين الرشقات"},
    {"faction", "Faction", "الفصيل"},
    {"add_faction", "Add faction", "إضافة فصيل"},
    {"team", "Team", "الفريق"},
    {"friendly_fire", "Friendly fire", "النيران الصديقة"},
    // --- Combat feedback authoring (Phase 59) -------------------------------
    // The Feedback panel was the one component in 0.6.0 with no inspector
    // section, so tuning a muzzle flash meant hand-editing the scene file. The
    // keys below name every field the line carries. Keys that already exist
    // ("enabled", "audio", "color", "intensity", "radius", "apply", "attach",
    // "detach") are deliberately NOT repeated: a second row would trip the
    // duplicate audit for no change in behaviour.
    {"feedback", "Combat Feedback", "تغذية القتال"},
    {"no_feedback", "No combat feedback on this entity.",
     "لا توجد تغذية قتال لهذا الكيان۔"},
    {"add_feedback", "Add combat feedback", "إضافة تغذية قتال"},
    {"flash_time", "Muzzle flash time", "زمن وميض الفوهة"},
    {"flash_size", "Muzzle flash size", "حجم وميض الفوهة"},
    {"tracer_time", "Tracer time", "زمن أثر الرصاصة"},
    {"tracer_length", "Tracer length", "طول أثر الرصاصة"},
    {"tracer_width", "Tracer width", "عرض أثر الرصاصة"},
    {"impact_time", "Scorch time", "زمن علامة الإصابة"},
    {"impact_size", "Scorch size", "حجم علامة الإصابة"},
    {"shot_volume", "Shot volume", "جهارة الطلقة"},
    {"impact_volume", "Impact volume", "جهارة الإصابة"},
    {"blast_time", "Blast flash time", "زمن وميض الانفجار"},
    {"blast_size", "Blast flash size", "حجم وميض الانفجار"},
    {"impact_volume_explosive", "Explosive impact volume", "جهارة إصابة الانفجار"},
    {"impact_volume_fire", "Fire impact volume", "جهارة إصابة النار"},
    {"muzzle_light", "Muzzle light", "ضوء الفوهة"},
    {"light_intensity", "Light intensity", "شدة الضوء"},
    {"light_radius", "Light radius", "نطاق الضوء"},
    {"light_color", "Light color", "لون الضوء"},
    {"max_flashes", "Max flashes", "أقصى الومضات"},
    {"max_tracers", "Max tracers", "أقصى آثار الرصاص"},
    {"max_impacts", "Max scorch marks", "أقصى علامات الإصابة"},
    {"max_sounds", "Max sounds", "أقصى الأصوات"},
    {"max_lights", "Max lights", "أقصى الأضواء"},
    // --- Perception authoring (Phase 60) ------------------------------------
    // The senses an NPC fights with. Sight carries its own five knobs because
    // that is the one an author tunes; the other senses are ranges, on the AI
    // module's documented defaults.
    {"perception", "Perception", "الإدراك"},
    {"no_perception", "No perception on this entity.", "لا يوجد إدراك لهذا الكيان۔"},
    {"add_perception", "Add perception", "إضافة إدراك"},
    {"sight_range", "Sight range", "مدى البصر"},
    {"sight_fov", "Sight cone (half angle)", "زاوية البصر (نصف)"},
    {"sight_gain", "Detect speed", "سرعة الانتباه"},
    {"sight_loss", "Forget speed", "سرعة النسيان"},
    {"sight_linger", "Search window", "نافذة البحث"},
    {"hearing_range", "Hearing range", "مدى السمع"},
    {"touch_range", "Touch range", "مدى اللمس"},
    {"pain_sense", "Feel pain", "الإحساس بالألم"},
    {"emit_noise", "Noise emitted", "الضجيج المنبعث"},
    {"visibility", "Visibility", "الظهور"},
    {"body_radius", "Body radius", "نصف قطر الجسم"},
    {"eye_height", "Eye height", "ارتفاع العين"},
    {"combat_senses", "Senses govern combat", "الحواس تحكم القتال"},
    {"perception_no_senses",
     "Every sense is off: this entity can never notice anything.",
     "كل الحواس مطفأة: هذا الكيان لن يلاحظ شيئًا أبدًا۔"},
    // --- Squad radio authoring (Phase 68) -----------------------------------
    // A radio shares the contacts its owner personally senses. The token is the
    // whole addressing scheme, so the field and its hint are the important ones.
    {"squad", "Squad radio", "راديو الفصيل"},
    {"no_squad", "No squad radio on this entity.", "لا يوجد راديو فصيل لهذا الكيان۔"},
    {"add_squad", "Add squad radio", "إضافة راديو فصيل"},
    {"squad_token", "Token", "الرمز"},
    {"squad_token_hint",
     "One case-sensitive word, at most 64 characters. Radios with the same token "
     "talk to each other.",
     "كلمة واحدة حساسة لحالة الأحرف، بطول 64 حرفًا كحد أقصى. الراديوات ذات الرمز "
     "نفسه تتخاطب."},
    {"squad_range", "Radio range (m)", "مدى الراديو (م)"},
    {"squad_range_hint",
     "A report is delivered only when BOTH radios reach: the distance must fit "
     "the reporter's range and the receiver's.",
     "لا يُسلَّم التقرير إلا عندما يبلغه الراديان معًا: يجب أن تقع المسافة داخل مدى "
     "المُبلِّغ والمستقبل."},
    {"squad_lost", "Lost callout (s)", "نداء الفقدان (ث)"},
    {"squad_lost_hint",
     "When a contact this entity personally sensed stops being sensed, it reports "
     "the last position once. Zero disables the callout.",
     "عندما ينقطع اتصال رصده هذا الكيان بنفسه، يبلّغ رفاقه بآخر موضع مرة واحدة. "
     "الصفر يعطّل النداء."},
    {"squad_cone", "Antenna cone (deg)", "زاوية الهوائي (درجة)"},
    {"squad_cone_hint",
     "Zero is omnidirectional. A positive half-angle makes this antenna talk and "
     "listen only inside its cone; a link needs BOTH ends to face each other.",
     "الصفر هوائي شامل الاتجاهات. الزاوية الموجبة تجعل هذا الهوائي يرسل ويستقبل "
     "داخل مخروطه فقط، ويحتاج الارتباط أن يتقابل الطرفان."},
    // --- Radio jammer authoring (Phase 70) -----------------------------------
    // A field that shortens the antennas inside it. Strength is a fraction, so
    // the two numbers read as "how big" and "how much", never as a range pair.
    {"jammer", "Radio jammer", "تشويش الراديو"},
    {"no_jammer", "No radio jammer on this entity.", "لا يوجد تشويش راديو لهذا الكيان۔"},
    {"add_jammer", "Add radio jammer", "إضافة تشويش راديو"},
    {"jammer_radius", "Jammer radius (m)", "نصف قطر التشويش (م)"},
    {"jammer_radius_hint",
     "Every squad radio inside this radius loses part of its reach. The strongest "
     "field covering an antenna wins; fields do not stack. Zero is inert.",
     "كل راديو فصيل داخل هذا النصف يخسر جزءًا من مداه. أقوى حقل يغطي الهوائي هو "
     "الذي يُطبَّق، والحقول لا تتراكم. الصفر معطّل."},
    {"jammer_strength", "Jammed fraction", "نسبة التشويش"},
    {"jammer_strength_hint",
     "The fraction of the radio range removed inside the field: 1 silences a "
     "radio, 0 keeps it at full range. A jammer jams its own side too.",
     "نسبة مدى الراديو المفقودة داخل الحقل: 1 يُسكت الراديو، و0 يُبقيه بمداه "
     "الكامل. التشويش يشمل فصيل صاحبه أيضًا."},
    // --- Decision layer authoring (Phase 61) --------------------------------
    // The considerations an NPC weighs. The input names are scene-format tokens
    // and they are translated here too, so the Arabic UI shows no Latin in a
    // value a designer reads.
    {"brain", "Brain", "العقل"},
    {"no_brain", "No decision maker on this entity.", "لا يوجد عقل لهذا الكيان۔"},
    {"add_brain", "Add decision maker", "إضافة عقل"},
    {"brain_rate", "Decide every (s)", "قرار كل (ث)"},
    {"brain_action", "Consideration", "اعتبار"},
    {"add_brain_action", "Add consideration", "إضافة اعتبار"},
    {"remove_brain_action", "Remove last", "حذف الأخير"},
    {"brain_input", "Input", "المُدخل"},
    {"brain_weight", "Weight", "الوزن"},
    {"brain_slope", "Slope", "الميل"},
    {"brain_exponent", "Exponent", "الأُس"},
    {"brain_input_offset", "Input offset", "إزاحة المُدخل"},
    {"brain_output_offset", "Output offset", "إزاحة الخرج"},
    {"hold_fire", "Hold fire", "حبس الزناد"},
    {"brain_in_aware", "aware", "واعٍ"},
    {"brain_in_sensing", "sensing", "يحسّ"},
    {"brain_in_distance", "distance", "المسافة"},
    {"brain_in_health", "health", "الصحة"},
    {"brain_in_ammo", "ammo", "الذخيرة"},
    {"brain_in_threat", "threat", "التهديد"},
    {"brain_in_enemies", "enemies", "الأعداء"},
    // --- State machine authoring (Phase 62) ---------------------------------
    {"states", "State machine", "آلة الحالة"},
    {"no_states", "No state machine on this entity.", "لا توجد آلة حالة لهذا الكيان۔"},
    {"add_states", "Add state machine", "إضافة آلة حالة"},
    {"states_rate", "Tick every (s)", "خطوة كل (ث)"},
    {"state_name", "State", "الحالة"},
    {"add_state", "Add state", "إضافة حالة"},
    {"remove_last", "Remove last", "حذف الأخير"},
    {"transition", "Transition", "انتقال"},
    {"from_state", "From", "من"},
    {"to_state", "To", "إلى"},
    {"guard_when", "When", "عندما"},
    {"guard_at", "At", "القيمة"},
    {"guard_below", "Below", "أدنى من"},
    // --- Keyboard shortcuts reference dialog & asset states -----------------
    {"shortcut_action", "Action", "الإجراء"},
    {"shortcut_key", "Shortcut", "المفتاح"},
    {"shortcut_general", "General", "عام"},
    {"shortcut_viewport", "Viewport & Navigation", "منفذ العرض والتنقل"},
    {"shortcut_tools", "Transform Tools", "أدوات التحويل"},
    {"shortcut_fly_cam", "Fly camera in scene", "تحليق الكاميرا في المشهد"},
    {"shortcut_f5", "Play / Stop", "تشغيل / إيقاف"},
    {"shortcut_focus", "Focus on selection", "تركيز على الكائن المحدد"},
    {"shortcut_select", "Select tool", "أداة التحديد"},
    {"asset_state_ready", "Ready", "جاهز"},
    {"asset_state_loading", "Loading...", "جارٍ التحميل..."},
    {"asset_state_unloaded", "Unloaded", "غير محمل"},
    {"asset_state_not_requested", "Not requested", "غير مطلوب"},
    // --- Context actions & asset creation -----------------------------------
    {"show_in_explorer", "Show in Explorer", "إظهار في مستكشف الملفات"},
    {"copy_log_message", "Copy message", "نسخ الرسالة"},
    {"create_child", "Create child entity", "إنشاء كائن فرعي"},
    {"new_material", "New Material", "خامة جديدة"},
    {"primitive", "Primitive", "مجسم أولي"},
    {"inspector_entity_gen", "Entity %u (gen %u)", "الكيان %u (الجيل %u)"},
    {"outliner_add_entity", "+ Add", "+ إضافة"},
    {"mode_2d_active", "2D Mode Active", "النمط ثنائي الأبعاد نشط"},
    {"mode_3d_active", "3D Mode Active", "النمط ثلاثي الأبعاد نشط"},
    {"cube", "Cube", "مكعب"},
    {"sphere", "Sphere", "كرة"},
    {"light_sun", "Sun Light", "شمس"},
    {"add_primitive", "Add Primitive", "إضافة مجسم"},
    {"sprite_2d", "2D Sprite", "كائن ثنائي الأبعاد"},
    {"point_light", "Point Light", "ضوء نقطي"},
    {"spot_light", "Spot Light", "ضوء كاشف"},
    {"audio_source", "Audio Source", "مصدر صوت"},
    {"quad", "Quad", "مربع مسطح"},
    {"flip_x", "Flip X", "عكس أفقي"},
    {"flip_y", "Flip Y", "عكس رأسي"},
    {"billboard", "Billboard", "مواجهة الكاميرا"},
    {"z_order", "Z-Order", "طبقة العرض"},
    {"inner_angle", "Inner Angle", "الزاوية الداخلية"},
    {"outer_angle", "Outer Angle", "الزاوية الخارجية"},
    {"tilemap_2d", "2D Tilemap", "خريطة مربعات ثنائية"},
    {"rigidbody_2d", "2D RigidBody", "جسم فيزيائي ثنائي"},
    {"collider_2d", "2D Collider", "مصادم ثنائي الأبعاد"},
    {"tile_size", "Tile Size", "حجم المربع"},
    {"columns", "Columns", "الأعمدة"},
    {"rows", "Rows", "الصفوف"},
    {"generate_collider", "Generate Collider", "توليد المصادم"},
    {"fixed_rotation", "Fixed Rotation", "تثبيت الدوران"},
    {"is_sensor", "Trigger / Sensor", "مستشعر فقط (بدون تصادم)"},
    {"filter_all", "All", "الكل"},
    {"filter_light", "Lights", "إضاءة"},
    {"filter_audio", "Audio", "صوت"},
    {"filter_physics", "Physics", "فيزياء"},
    {"grid", "Grid", "شبكة"},
    {"copy_transform", "Copy TRS", "نسخ التحويل"},
    {"paste_transform", "Paste TRS", "لصق التحويل"},
    {"reset_transform", "Reset TRS", "تصفير شامل"},
    {"copy_all_logs", "Copy All Logs", "نسخ كل السجلات"},
    {"preset_sun", "Sun", "شمس"},
    {"preset_fire", "Torch", "نار"},
    {"preset_neon", "Neon", "نيون"},
    {"preset_emerald", "Emerald", "زمرد"},
    {"preset_moon", "Moon", "قمر"},
    // --- Phases 115-122: Editor Integration ---
    {"weather_system", "Weather & Atmosphere", "الطقس والغلاف الجوي"},
    {"weather_preset", "Weather Preset", "النمط الجوي"},
    {"weather_clear", "Clear Sky", "صحو مشمس"},
    {"weather_cloudy", "Partly Cloudy", "غائم جزئياً"},
    {"weather_overcast", "Overcast", "ملبد بالغيوم"},
    {"weather_rain", "Rainy", "ماطر"},
    {"weather_thunderstorm", "Thunderstorm", "عاصفة رعدية"},
    {"weather_foggy", "Dense Fog", "ضباب كثيف"},
    {"weather_snow", "Snowy", "مثلج"},
    {"fog_density", "Fog Density", "كثافة الضباب"},
    {"fog_falloff", "Height Falloff", "تلاشي الارتفاع"},
    {"fog_base_height", "Base Height", "ارتفاع الأساس"},
    {"fog_max_opacity", "Max Opacity", "أقصى عتامة"},
    {"sun_inscattering", "Sun Scattering", "تشتت الشمس"},
    {"ground_wetness", "Ground Wetness", "بلل الأرض والبرك"},
    {"wind_strength", "Wind Speed", "سرعة الرياح"},
    {"wind_direction", "Wind Direction", "اتجاه الرياح"},
    {"cable_physics", "Cable / Rope Physics", "فيزياء الكابل والحبل"},
    {"total_length", "Total Length", "الطول الإجمالي"},
    {"particle_count", "Particle Count", "عدد الجسيمات"},
    {"cable_stiffness", "Cable Stiffness", "مرونة الكابل"},
    {"linear_density", "Linear Density", "الكثافة الخطية"},
    {"max_tension", "Max Tension (N)", "أقصى شد (نيوتن)"},
    {"live_tension", "Live Tension", "الشد اللحظي"},
    {"breakable", "Breakable", "قابل للانقطاع"},
    {"pin_start", "Pin Start", "تثبيت البداية"},
    {"pin_end", "Pin End", "تثبيت النهاية"},
    {"end_offset", "End Offset", "إزاحة النهاية"},
    {"rule_tile", "2D Rule Tile", "بلاط القواعد الذكي"},
    {"rule_preset", "Auto-Tile Preset", "نمط البلاط التلقائي"},
    {"rule_preset_16", "16-Tile Cardinal", "16 بلاطة - اتجاهات رئيسية"},
    {"rule_preset_47", "47-Blob Wang Autotile", "47 بلاطة - تشكيلات وانغ"},
    {"rule_preset_custom", "Custom Pattern", "قواعد مخصصة"},
    {"rule_count", "Rule Count", "عدد القواعد"},
    {"auto_update_map", "Auto Update Tilemap", "تحديث تلقائي للخريطة"},
    {"neighbor_pattern", "3x3 Neighbor Pattern", "نمط الجيران 3×3"},
    {"flow_field", "Flow Field Crowd Nav", "حقل تدفق الحشود"},
    {"grid_width", "Grid Width", "عرض الشبكة"},
    {"grid_height", "Grid Height", "ارتفاع الشبكة"},
    {"target_x", "Target X", "الهدف س"},
    {"target_y", "Target Y", "الهدف ص"},
    {"recalculate_flow", "Recalculate Flow", "إعادة حساب التدفق"},
    {"talent_tree", "Talent Progression Tree", "شجرة المواهب والمهارات"},
    {"tree_id", "Tree ID", "معرّف الشجرة"},
    {"tree_name", "Tree Name", "اسم الشجرة"},
    {"available_points", "Available Points", "النقاط المتاحة"},
    {"spent_points", "Spent Points", "النقاط المستثمرة"},
    {"upgrade_rank", "Invest Point (+)", "ترقية (+)"},
    {"refund_rank", "Refund Point (-)", "استرداد (-)"},
    {"ragdoll_blend", "Ragdoll Blend & Get-Up", "مزج راجدول والنهوض"},
    {"blend_weight", "Blend Weight", "وزن المزج"},
    {"pose_state", "Pose State", "حالة الوضعية"},
    {"pose_inactive", "Keyframed (0)", "حركي اعتيادي"},
    {"pose_ragdoll", "Full Ragdoll (1)", "فيزيائي كامل"},
    {"pose_getup_face_up", "Get-Up Face Up (2)", "نهوض من الظهر"},
    {"pose_getup_face_down", "Get-Up Face Down (3)", "نهوض من البطن"},
    {"preserve_momentum", "Preserve Momentum", "الحفاظ على الزخم"},
    {"grid_inventory", "2D Grid Inventory", "حقيبة التخزين الشبكية"},
    {"inv_columns", "Grid Columns", "الأعمدة"},
    {"inv_rows", "Grid Rows", "الصفوف"},
    {"max_weight", "Max Capacity", "السعة القصوى"},
    {"current_weight", "Current Weight", "الوزن الحالي"},
    {"auto_pack", "Auto-Pack Items", "ترتيب تلقائي للحقيبة"},
    {"surface_audio", "Surface Material Audio", "صوتيات سطح المادة والخطوات"},
    {"surface_material", "Surface Material", "مادة السطح"},
    {"mat_concrete", "Concrete", "خرسانة"},
    {"mat_dirt", "Dirt", "تراب"},
    {"mat_metal", "Metal", "معدن"},
    {"mat_wood", "Wood", "خشب"},
    {"mat_water", "Water", "ماء"},
    {"mat_grass", "Grass", "عشب"},
    {"mat_snow", "Snow", "ثلج"},
    {"pitch_variation", "Pitch Variation", "تنويع النبرة"},
    {"wetness_modulation", "Wetness Modulation", "تعديل رطوبة السطح"},
    {"add_weather", "Weather & Atmosphere", "طقس وبيئة جوية"},
    {"add_cable", "Physics Cable", "كابل فيزيائي"},
    {"add_rule_tile", "2D Rule Tile", "بلاط القواعد"},
    {"add_flow_field", "Flow Field Nav", "حقل تدفق الحشود"},
    {"add_talent_tree", "Talent Tree", "شجرة مواهب"},
    {"tab_weather", "Weather", "الطقس"},
    {"tab_talent_tree", "Talent Tree", "شجرة المواهب"},
    {"tab_flow_field", "Crowd Flow", "حقل الحشود"},
    {"scene_hub_title", "Scene Systems & Environment Dashboard", "مركز تحكم المشهد والأنظمة الذكية"},
    {"quick_weather_title", "Quick Weather & Atmospheric Fog", "التحكم السريع بالطقس والضباب"},
    {"quick_weather_hint", "Quickly change scene weather & atmosphere:", "تغيير سريع لبيئة وطقس المشهد بنقرة واحدة:"},
    {"quick_create_title", "Quick-Spawn Advanced Systems", "إضافة وتجهيز الأنظمة المتقدمة فورياً"},
    {"quick_add_component", "Quick-add advanced components:", "إضافة سريعة للمكونات المتقدمة:"},
    {"active_systems_census", "Active Advanced Scene Systems:", "الأنظمة المتقدمة النشطة في المشهد:"},
    {"no_active_systems", "No advanced systems active yet", "لا توجد أنظمة متقدمة نشطة بعد"},
    {"tab_cable_sim", "Cable Simulation", "محاكاة الكابلات والحبال"},
    {"weather_hud", "Scene Weather", "طقس المشهد"},
    {"filter_weather", "Weather", "طقس"},
    {"filter_cable", "Cables", "كابلات"},
    {"filter_nav", "Nav", "ملاحة"},
    {"filter_talents", "Talents", "مهارات"},
    {"snap_cable_test", "Snap Cable Test", "اختبار قطع الكابل"},
    {"reset_cable_ends", "Reset Ends", "إعادة ضبط الأطراف"},
    {"apply_weather_scene", "Apply Weather to Scene", "تطبيق الطقس على المشهد"},
    {"cable_sim", "Cable Simulation", "محاكاة الكابلات"},
    {"wind_speed", "Wind Speed", "سرعة الرياح"},
    {"rest_length", "Rest Length", "الطول الطبيعي"},
    {"tension_gauge", "Tension Gauge", "مقياس الشد"},
};

// The active language. Atomic: set_language() runs on the UI/main thread
// while tr() runs on the render thread (tr() is called per widget per
// frame), so a plain global here is a data race.
std::atomic<Language> g_language{Language::English};

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
    g_language.store(lang, std::memory_order_relaxed);
}

Language current_language() {
    return g_language.load(std::memory_order_relaxed);
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
    if (g_language.load(std::memory_order_relaxed) == Language::Arabic &&
        e->ar != nullptr && e->ar[0] != '\0') {
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
