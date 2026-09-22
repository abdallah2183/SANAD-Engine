// Tests/ToolTests/test_templates.cpp — G7 game-start templates.
//
// Each template under Templates/<Name> must scaffold into a project whose
// startup scene (Content/Scenes/Main.nfscene) loads clean and actually plays:
// a camera, a light, animated entities, dynamic physics bodies, and audio.
// "Plays" here is checked on the scene data the same way the player's own
// acceptance telemetry judges a run — an entity that cannot move and a sound
// that cannot mix are failures, not ambience.
//
// The second half of the file goes one step further: it cooks and packages the
// scaffolded project with the real NFPlayer and launches it. A template that
// loads in-process but cannot be built into a runnable game is not a playable
// template, and "playable in under a minute from Create project" is a claim
// about the built artifact, not about the scene file.

#include <NF/Test/TestFramework.hpp>

#include <NF/Project/ProjectScaffold.hpp>
#include <NF/Project/ProjectPackager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Scene/Transform.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// No <windows.h> here on purpose: rpcdce.h declares a global `UUID` typedef that
// makes every unqualified `UUID` in this file ambiguous with `nf::UUID`.

// The framework ships NF_CHECK / NF_CHECK_EQ / NF_CHECK_NEAR, none of which
// carry a message. One shared helper below runs for three templates, so a bare
// "NF_CHECK failed" would not say which one broke; this local variant does.
// Deliberately file-local: the shared header is not this track's to grow.
#define NF_CHECK_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(std::string(msg)); \
        } \
    } while (0)

using namespace nf;
using namespace nf::project;
using namespace nf::assets;
using namespace nf::runtime;

#ifndef NF_TEMPLATE_THIRDPERSON_DIR
    #define NF_TEMPLATE_THIRDPERSON_DIR ""
#endif
#ifndef NF_TEMPLATE_FPSSTARTER_DIR
    #define NF_TEMPLATE_FPSSTARTER_DIR ""
#endif
#ifndef NF_TEMPLATE_PLATFORMER2D_DIR
    #define NF_TEMPLATE_PLATFORMER2D_DIR ""
#endif

namespace {

struct TemplateSpec {
    const char* template_dir;
    const char* label;
    size_t expected_entities;
};

std::filesystem::path scaffold_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    return p;
}

// Scaffolds a project from one template and returns the scaffolded root.
// NF_CHECK failures name the template, so a regression points at the right one.
std::filesystem::path scaffold_from(const TemplateSpec& spec, const std::string& scratch) {
    const auto root = scaffold_dir(scratch);
    ScaffoldOptions opts;
    opts.root = root;
    opts.name = "Game";
    opts.template_dir = spec.template_dir;
    std::string err;
    const bool ok = scaffold_project(opts, err);
    if (!ok) {
        // Surface the scaffold error in the assertion text.
        NF_CHECK_MSG(false, (std::string(spec.label) + ": scaffold failed: " + err).c_str());
    }
    return root;
}

SceneLoadResult load_main_scene(const std::filesystem::path& root) {
    return load_scene_from_physical(root / "Content" / "Scenes" / "Main.nfscene");
}

// The shared acceptance: whatever the genre, a template scene must open with a
// view, a light, motion, physics that settles, and sound.
void check_template_plays(const TemplateSpec& spec, const std::filesystem::path& root) {
    SceneLoadResult loaded = load_main_scene(root);
    NF_CHECK_MSG(loaded.success,
                 (std::string(spec.label) + ": Main.nfscene failed to load: " + loaded.error).c_str());
    if (!loaded.success || !loaded.scene) return;
    NF_CHECK_MSG(loaded.warnings.empty(),
                 (std::string(spec.label) + ": scene load produced warnings (" +
                  std::to_string(loaded.warnings.size()) + "), first: " +
                  (loaded.warnings.empty() ? std::string() : loaded.warnings.front())).c_str());

    auto& world = loaded.scene->world();
    const auto entities = world.all_entities();
    NF_CHECK_MSG(entities.size() == spec.expected_entities,
                 (std::string(spec.label) + ": expected " + std::to_string(spec.expected_entities) +
                  " entities, scene has " + std::to_string(entities.size())).c_str());

    // A view: exactly one active camera.
    size_t active_cameras = 0;
    for (auto e : world.query<CameraComponent>()) {
        const auto* c = world.get<CameraComponent>(e);
        if (c != nullptr && c->is_active) ++active_cameras;
    }
    NF_CHECK_EQ(active_cameras, size_t(1));

    // A light.
    NF_CHECK(!world.query<DirectionalLight>().empty());

    // Motion: every animated entity carries a real procedural clip — a bare
    // clip name with no data samples the rest pose forever (no import pipeline).
    size_t animated = 0;
    for (auto e : world.query<animation::AnimationComponent>()) {
        const auto* a = world.get<animation::AnimationComponent>(e);
        if (a == nullptr) continue;
        NF_CHECK_MSG(a->has_procedural,
                     (std::string(spec.label) + ": entity " + std::to_string(e.id) +
                      " animates a clip with no data").c_str());
        NF_CHECK(!a->clips.empty());
        ++animated;
    }
    NF_CHECK_MSG(animated > 0, (std::string(spec.label) + ": nothing animates").c_str());

    // Physics that settles: at least one dynamic body over a static floor.
    size_t dynamic_bodies = 0;
    size_t static_bodies = 0;
    for (auto e : world.query<physics::RigidBodyComponent>()) {
        const auto* rb = world.get<physics::RigidBodyComponent>(e);
        if (rb == nullptr) continue;
        if (rb->type == physics::BodyType::Dynamic) ++dynamic_bodies;
        if (rb->type == physics::BodyType::Static) ++static_bodies;
    }
    NF_CHECK_MSG(dynamic_bodies > 0, (std::string(spec.label) + ": nothing falls").c_str());
    NF_CHECK_MSG(static_bodies > 0, (std::string(spec.label) + ": nothing to land on").c_str());

    // Sound: at least one tone buffer synthesised at load (no audio files needed).
    size_t audible = 0;
    for (auto e : world.query<audio::AudioComponent>()) {
        const auto* a = world.get<audio::AudioComponent>(e);
        if (a == nullptr) continue;
        if (a->tone_hz > 0.0f && !a->owned_buffer.samples.empty()) ++audible;
    }
    NF_CHECK_MSG(audible > 0, (std::string(spec.label) + ": nothing can be heard").c_str());
}

// The scene references meshes by AssetId; the template's registry must resolve
// every one of them or the first cook produces a game with missing geometry.
void check_registry_resolves_scene_meshes(const TemplateSpec& spec, const std::filesystem::path& root) {
    SceneLoadResult loaded = load_main_scene(root);
    NF_CHECK(loaded.success && loaded.scene != nullptr);
    if (!loaded.success || !loaded.scene) return;

    std::set<std::string> referenced;
    for (auto e : loaded.scene->world().query<MeshComponent>()) {
        if (const auto* m = loaded.scene->world().get<MeshComponent>(e)) {
            referenced.insert(m->mesh_id.to_string());
        }
    }
    NF_CHECK(!referenced.empty());

    AssetRegistry registry;
    std::string err;
    NF_CHECK_MSG(registry.load_from_physical(root / "Content" / "AssetRegistry.nfreg", err),
                 (std::string(spec.label) + ": template registry failed to load: " + err).c_str());
    for (const std::string& id_str : referenced) {
        const UUID uuid = UUID::from_string(id_str);
        NF_CHECK_MSG(uuid.is_valid(), (std::string(spec.label) + ": scene has an invalid mesh id " + id_str).c_str());
        NF_CHECK_MSG(registry.find(AssetId(uuid)) != nullptr,
                     (std::string(spec.label) + ": registry has no entry for mesh " + id_str).c_str());
    }
}

void check_project_descriptor(const std::filesystem::path& root) {
    std::string err;
    auto desc = ProjectDescriptor::load_from_file(root / "Game.nfproj", err);
    NF_CHECK(desc.has_value());
    if (desc) {
        NF_CHECK_EQ(desc->startup_scene(), std::string("content://Scenes/Main.nfscene"));
        NF_CHECK(desc->has_mount("content://"));
        NF_CHECK(desc->has_mount("cache://"));
        NF_CHECK(desc->has_mount("shaders://"));
    }
}

void run_template_suite(const TemplateSpec& spec, const std::string& scratch) {
    if (std::string(spec.template_dir).empty()) {
        NF_SKIP((std::string(spec.label) + " built without a template dir define").c_str());
    }
    const auto root = scaffold_from(spec, scratch);
    check_project_descriptor(root);
    check_template_plays(spec, root);
    check_registry_resolves_scene_meshes(spec, root);
    // A mesh the scene references must exist on disk in the scaffolded content.
    NF_CHECK(std::filesystem::exists(root / "Content" / "Meshes" / "cube.nfmesh"));
    NF_CHECK(std::filesystem::exists(root / "Content" / "Meshes" / "sphere.nfmesh"));
    NF_CHECK(std::filesystem::exists(root / "Content" / "Materials" / "Default.nfmat"));
    std::filesystem::remove_all(root);
}

const TemplateSpec kThirdPerson{
    NF_TEMPLATE_THIRDPERSON_DIR, "ThirdPerson", 12};
const TemplateSpec kFpsStarter{
    NF_TEMPLATE_FPSSTARTER_DIR, "FPSStarter", 15};
const TemplateSpec kPlatformer2D{
    NF_TEMPLATE_PLATFORMER2D_DIR, "Platformer2D", 16};

} // namespace

NF_TEST(template_thirdperson_scaffolds_into_a_playable_project) {
    run_template_suite(kThirdPerson, "nf_tpl_thirdperson");
}

NF_TEST(template_fpsstarter_scaffolds_into_a_playable_project) {
    run_template_suite(kFpsStarter, "nf_tpl_fpsstarter");
}

NF_TEST(template_platformer2d_scaffolds_into_a_playable_project) {
    run_template_suite(kPlatformer2D, "nf_tpl_platformer2d");
}

NF_TEST(template_default_is_still_green) {
    // The Default template ships to every `nf new`; the three game templates
    // must not have disturbed it.
    const TemplateSpec def{NF_TEMPLATE_DIR, "Default", 6};
    if (std::string(def.template_dir).empty()) {
        NF_SKIP("built without NF_TEMPLATE_DIR");
    }
    const auto root = scaffold_from(def, "nf_tpl_default");
    SceneLoadResult loaded = load_main_scene(root);
    NF_CHECK_MSG(loaded.success,
                 (std::string("Default: Main.nfscene failed to load: ") + loaded.error).c_str());
    if (loaded.success && loaded.scene) {
        NF_CHECK_EQ(loaded.scene->world().all_entities().size(), def.expected_entities);
    }
    std::filesystem::remove_all(root);
}

// ---------------------------------------------------------------------------
// Packaged playback — the template as a built, running game
// ---------------------------------------------------------------------------
//
// Everything above judges the data the scaffold copies. This half judges the
// artifact the developer gets: scaffold -> cook -> package -> NFPlayer.
//
// The assertions read the runtime's OWN acceptance telemetry rather than
// trusting exit 0, because a scene that loads and draws nothing is a failure
// the runtime already refuses to call clean:
//
//   "scene loaded 'content://Scenes/Main.nfscene' with N entities"
//   "Rendered F frames (meshes=M)"                  M > 0  (assets really cooked)
//   "Animated entities: A (max transform deviation over F frames: D)"
//                                                   A > 0, D > 0 (motion is driven)
//   "Audio sources mixed (peak): C (P)"              C > 0, P > 0 (sound is mixed)
//   "Alive RHI objects before shutdown: 0"
//   "=== NOVAForge Runtime exited cleanly ==="

#ifndef NF_PLAYER_EXE
    #define NF_PLAYER_EXE ""
#endif
#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

namespace {

constexpr long kPlayFrames = 60;

struct PlayerRun {
    int exit_code = -1;
    std::string log;
};

// Runs the packaged player with its working directory set to the package.
//
// The command deliberately starts with `cd`, not a quote: cmd.exe strips the
// outer quotes of a command that begins with one, which turns a quoted exe path
// into garbage. The log is written outside the package so the package tree is
// left exactly as the player made it.
PlayerRun run_packaged_player(const std::filesystem::path& package_dir) {
    const auto log_path = package_dir.parent_path() / (package_dir.filename().string() + "_play.log");
    const std::string args = "--frames " + std::to_string(kPlayFrames) + " --headless";
    const std::string cmd = "cd /d \"" + package_dir.string() + "\" && \"" +
                            (package_dir / "NFPlayer.exe").string() + "\" " + args +
                            " > \"" + log_path.string() + "\" 2>&1";

    PlayerRun run;
    run.exit_code = std::system(cmd.c_str());
    if (std::ifstream in(log_path, std::ios::binary); in) {
        run.log.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
    return run;
}

// Every number on the line containing `key`, in the order the line prints them.
// The runtime's telemetry lines are fixed shapes, so the caller can index them
// (documented at each call site) instead of re-deriving a format each time.
std::vector<double> numbers_after(const std::string& log, const std::string& key) {
    std::vector<double> out;
    const std::size_t at = log.find(key);
    if (at == std::string::npos) {
        return out;
    }
    const std::size_t begin = at + key.size();
    const std::size_t eol = log.find('\n', begin);
    const std::string line = log.substr(begin, eol == std::string::npos ? std::string::npos : eol - begin);

    const char* p = line.c_str();
    while (*p != '\0') {
        char* end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(value);
        p = end;
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void check_template_builds_and_plays(const TemplateSpec& spec, const std::string& scratch) {
    const std::string label = spec.label;
    if (std::string(spec.template_dir).empty()) {
        NF_SKIP((label + " built without a template dir define").c_str());
    }
    if (std::string(NF_PLAYER_EXE).empty()) {
        NF_SKIP("NFPlayer not built (NF_BUILD_TOOLS=OFF)");
    }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) {
        NF_SKIP("NF_BASIC3D_SHADER_DIR not configured");
    }

    const auto root = scaffold_from(spec, scratch);

    BuildOptions opts;
    opts.project_file = root / "Game.nfproj";
    opts.shader_dir = std::string(NF_BASIC3D_SHADER_DIR);
    opts.player_exe = std::string(NF_PLAYER_EXE);
    BuildReport report;
    std::string err;
    const bool built = build_project(opts, report, err);
    NF_CHECK_MSG(built, (label + ": cook/package failed: " + err).c_str());
    if (!built) return;

    // A template whose assets do not cook ships a game with no geometry.
    NF_CHECK_MSG(report.cook.failed == 0,
                 (label + ": " + std::to_string(report.cook.failed) + " asset(s) failed to cook").c_str());
    NF_CHECK_MSG(report.cook.total() > 0, (label + ": nothing cooked").c_str());

    const auto package_dir = root / "dist";
    NF_CHECK_MSG(std::filesystem::exists(package_dir / "NFPlayer.exe"),
                 (label + ": package has no NFPlayer.exe").c_str());

    // No --project: the package must describe itself from beside the player.
    const PlayerRun run = run_packaged_player(package_dir);
    const std::string& log = run.log;

    NF_CHECK_MSG(run.exit_code == 0,
                 (label + ": packaged player exited " + std::to_string(run.exit_code) +
                  "\n--- player log ---\n" + log).c_str());
    NF_CHECK_MSG(contains(log, "=== NOVAForge Runtime exited cleanly ==="),
                 (label + ": runtime did not report a clean exit\n--- player log ---\n" + log).c_str());

    // The startup scene the scaffold's descriptor points at is the one that ran.
    NF_CHECK_MSG(contains(log, "scene loaded 'content://Scenes/Main.nfscene' with " +
                                   std::to_string(spec.expected_entities) + " entities"),
                 (label + ": the packaged player did not load Main.nfscene with " +
                  std::to_string(spec.expected_entities) + " entities\n--- player log ---\n" + log).c_str());

    // "Rendered F frames (meshes=M)" -> [frames, meshes].
    const auto rendered = numbers_after(log, "Rendered ");
    NF_CHECK_MSG(rendered.size() >= 2, (label + ": no render telemetry\n--- player log ---\n" + log).c_str());
    if (rendered.size() >= 2) {
        NF_CHECK_MSG(static_cast<long>(rendered[0]) == kPlayFrames,
                     (label + ": asked for " + std::to_string(kPlayFrames) + " frames, ran " +
                      std::to_string(static_cast<long>(rendered[0]))).c_str());
        NF_CHECK_MSG(rendered[1] > 0.0,
                     (label + ": the cooked package uploaded no meshes — geometry is missing").c_str());
    }

    // "Animated entities: A (max transform deviation over F frames: D)" ->
    // [animated, frames, deviation]. A pose that is computed but never reaches
    // the entity is the failure this catches.
    const auto anim = numbers_after(log, "Animated entities: ");
    NF_CHECK_MSG(!anim.empty() && anim[0] > 0.0,
                 (label + ": the packaged game has no animated entity\n--- player log ---\n" + log).c_str());
    NF_CHECK_MSG(anim.size() >= 3 && anim[2] > 1e-3,
                 (label + ": animated entities exist but no driven transform moved").c_str());

    // "Audio sources mixed (peak): C (P)" -> [sources, peak].
    const auto audio = numbers_after(log, "Audio sources mixed (peak): ");
    NF_CHECK_MSG(audio.size() >= 2 && audio[0] > 0.0 && audio[1] > 0.0,
                 (label + ": the packaged game mixed no audio\n--- player log ---\n" + log).c_str());

    NF_CHECK_MSG(contains(log, "Alive RHI objects before shutdown: 0"),
                 (label + ": the player leaked RHI objects\n--- player log ---\n" + log).c_str());

    std::filesystem::remove_all(root);
}

const TemplateSpec kThirdPersonPlay{NF_TEMPLATE_THIRDPERSON_DIR, "ThirdPerson", 12};
const TemplateSpec kFpsStarterPlay{NF_TEMPLATE_FPSSTARTER_DIR, "FPSStarter", 15};
const TemplateSpec kPlatformer2DPlay{NF_TEMPLATE_PLATFORMER2D_DIR, "Platformer2D", 16};

} // namespace

NF_TEST(template_thirdperson_builds_and_plays_in_the_shipped_player) {
    check_template_builds_and_plays(kThirdPersonPlay, "nf_tpl_tp_play");
}

NF_TEST(template_fpsstarter_builds_and_plays_in_the_shipped_player) {
    check_template_builds_and_plays(kFpsStarterPlay, "nf_tpl_fps_play");
}

NF_TEST(template_platformer2d_builds_and_plays_in_the_shipped_player) {
    check_template_builds_and_plays(kPlatformer2DPlay, "nf_tpl_2d_play");
}
