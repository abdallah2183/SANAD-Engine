// Tests/ToolTests/test_model_importer_cli.cpp — the shipped NFModelImporter CLI.
//
// The import API is pinned next door in Tests/AssetTests (glTF character round
// trip). This file pins the half a developer actually touches: the command
// `Docs/Blender_Pipeline.md` tells them to run. It writes a real .glb to disk —
// the exact container the shipped Blender add-on exports (`export_format="GLB"`)
// — then runs the real NFModelImporter.exe as a subprocess and asserts the
// report it prints.
//
// The point of the report assertions is the project's hard rule: no silent
// format substitution. A developer must be able to see, from the console, that
// the skin, the joints and both clips arrived, and that the one unsupported
// channel was counted rather than dropped. If any of those lines go missing,
// this file goes red.

#include <NF/Assets/MeshAsset.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace nf;

#ifndef NF_MODEL_IMPORTER_EXE
    #define NF_MODEL_IMPORTER_EXE ""
#endif

namespace {

// ---------------------------------------------------------------------------
// GLB container writer (glTF 2.0 binary, single embedded BIN chunk)
// ---------------------------------------------------------------------------

void push_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFFu));
    b.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    b.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    b.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

void push_f32(std::vector<u8>& b, float v) {
    u32 u = 0;
    std::memcpy(&u, &v, 4);
    push_u32(b, u);
}

// Header (magic/version/length) + JSON chunk (space-padded) + BIN chunk.
std::vector<u8> make_glb(const std::string& json_in, const std::vector<u8>& bin) {
    std::string json = json_in;
    while (json.size() % 4 != 0) json.push_back(' ');
    std::vector<u8> bin_padded = bin;
    while (bin_padded.size() % 4 != 0) bin_padded.push_back(static_cast<u8>(0));

    std::vector<u8> out;
    push_u32(out, 0x46546C67u); // "glTF"
    push_u32(out, 2u);
    push_u32(out, static_cast<u32>(12 + 8 + json.size() + 8 + bin_padded.size()));
    push_u32(out, static_cast<u32>(json.size()));
    push_u32(out, 0x4E4F534Au); // "JSON"
    out.insert(out.end(), json.begin(), json.end());
    push_u32(out, static_cast<u32>(bin_padded.size()));
    push_u32(out, 0x004E4942u); // "BIN\0"
    out.insert(out.end(), bin_padded.begin(), bin_padded.end());
    return out;
}

// A game-ready character: one skinned mesh (u8 JOINTS_0 / normalized u8
// WEIGHTS_0), a 1-joint skin, two sampled clips, one CUBICSPLINE clip the
// importer must count as skipped, and a Principled material.
//
// Buffer layout (offsets are the ones the JSON below names):
//   0   POSITION   3 x VEC3 f32   (36)
//   36  JOINTS_0   3 x VEC4 u8    (12)
//   48  WEIGHTS_0  3 x VEC4 u8    (12)
//   60  indices    3 x u8         (3)
//   64  idle in    2 x f32        (8)
//   72  idle out   2 x VEC3 f32   (24)
//   96  wave in    2 x f32        (8)
//   104 wave out   2 x VEC4 f32   (32)
//   136 spl in     2 x f32        (8)
//   144 spl out    6 x VEC3 f32   (72)
//   = 216 bytes
const char* kCharacterJson = R"JSON({"asset":{"version":"2.0"},
"buffers":[{"byteLength":216}],
"bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":36},
{"buffer":0,"byteOffset":36,"byteLength":12},
{"buffer":0,"byteOffset":48,"byteLength":12},
{"buffer":0,"byteOffset":60,"byteLength":3},
{"buffer":0,"byteOffset":64,"byteLength":8},
{"buffer":0,"byteOffset":72,"byteLength":24},
{"buffer":0,"byteOffset":96,"byteLength":8},
{"buffer":0,"byteOffset":104,"byteLength":32},
{"buffer":0,"byteOffset":136,"byteLength":8},
{"buffer":0,"byteOffset":144,"byteLength":72}],
"accessors":[
{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
{"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
{"bufferView":2,"componentType":5121,"count":3,"type":"VEC4","normalized":true},
{"bufferView":3,"componentType":5121,"count":3,"type":"SCALAR"},
{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"},
{"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":7,"componentType":5126,"count":2,"type":"VEC4"},
{"bufferView":8,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":9,"componentType":5126,"count":6,"type":"VEC3"}],
"materials":[{"name":"HeroMat","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.4,0.2,1.0],"metallicFactor":0.1,"roughnessFactor":0.7}}],
"meshes":[{"name":"HeroMesh","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,"material":0,"mode":4}]}],
"skins":[{"name":"HeroRig","joints":[1]}],
"nodes":[{"name":"Body","mesh":0,"skin":0,"children":[1]},{"name":"Bone0","translation":[0,0.5,0]}],
"animations":[
{"name":"Idle","samplers":[{"input":4,"output":5}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]},
{"name":"Wave","samplers":[{"input":6,"output":7}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]},
{"name":"Splined","samplers":[{"interpolation":"CUBICSPLINE","input":8,"output":9}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}],
"scenes":[{"nodes":[0]}],"scene":0})JSON";

// The strict-validation fixture is the same character with no extra unsupported
// clip. Unused buffer views/accessors are legal and keep the binary layout
// shared with the report fixture above.
const char* kStrictCharacterJson = R"JSON({"asset":{"version":"2.0"},
"buffers":[{"byteLength":216}],
"bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":36},
{"buffer":0,"byteOffset":36,"byteLength":12},
{"buffer":0,"byteOffset":48,"byteLength":12},
{"buffer":0,"byteOffset":60,"byteLength":3},
{"buffer":0,"byteOffset":64,"byteLength":8},
{"buffer":0,"byteOffset":72,"byteLength":24},
{"buffer":0,"byteOffset":96,"byteLength":8},
{"buffer":0,"byteOffset":104,"byteLength":32},
{"buffer":0,"byteOffset":136,"byteLength":8},
{"buffer":0,"byteOffset":144,"byteLength":72}],
"accessors":[
{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
{"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
{"bufferView":2,"componentType":5121,"count":3,"type":"VEC4","normalized":true},
{"bufferView":3,"componentType":5121,"count":3,"type":"SCALAR"},
{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"},
{"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":7,"componentType":5126,"count":2,"type":"VEC4"},
{"bufferView":8,"componentType":5126,"count":2,"type":"SCALAR"},
{"bufferView":9,"componentType":5126,"count":6,"type":"VEC3"}],
"materials":[{"name":"HeroMat","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.4,0.2,1.0],"metallicFactor":0.1,"roughnessFactor":0.7}}],
"meshes":[{"name":"HeroMesh","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,"material":0,"mode":4}]}],
"skins":[{"name":"HeroRig","joints":[1]}],
"nodes":[{"name":"Body","mesh":0,"skin":0,"children":[1]},{"name":"Bone0","translation":[0,0.5,0]}],
"animations":[
{"name":"Idle","samplers":[{"input":4,"output":5}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]},
{"name":"Wave","samplers":[{"input":6,"output":7}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]}],
"scenes":[{"nodes":[0]}],"scene":0})JSON";

std::vector<u8> character_bin() {
    std::vector<u8> b;
    push_f32(b, 0.0f); push_f32(b, 0.0f); push_f32(b, 0.0f);
    push_f32(b, 1.0f); push_f32(b, 0.0f); push_f32(b, 0.0f);
    push_f32(b, 0.0f); push_f32(b, 1.0f); push_f32(b, 0.0f);
    for (int i = 0; i < 12; ++i) b.push_back(static_cast<u8>(0)); // JOINTS_0
    const u8 w[12] = {255, 0, 0, 0, 128, 128, 0, 0, 0, 255, 0, 0}; // WEIGHTS_0
    b.insert(b.end(), w, w + 12);
    b.push_back(static_cast<u8>(0));
    b.push_back(static_cast<u8>(1));
    b.push_back(static_cast<u8>(2));
    b.push_back(static_cast<u8>(0)); // pad to the next 4-byte offset
    push_f32(b, 0.0f); push_f32(b, 1.0f); // idle input
    push_f32(b, 0.0f); push_f32(b, 0.5f); push_f32(b, 0.0f);
    push_f32(b, 0.0f); push_f32(b, 1.0f); push_f32(b, 0.0f);
    push_f32(b, 0.0f); push_f32(b, 1.0f); // wave input
    push_f32(b, 0.0f); push_f32(b, 0.0f); push_f32(b, 0.0f); push_f32(b, 1.0f);
    push_f32(b, 0.0f); push_f32(b, 0.0f); push_f32(b, 0.70710678f); push_f32(b, 0.70710678f);
    push_f32(b, 0.0f); push_f32(b, 1.0f); // spline input
    for (int i = 0; i < 18; ++i) push_f32(b, 0.0f); // in-tangent/value/out-tangent
    return b;
}

// A prop: one unindexed triangle, no skin, no animation.
const char* kStaticJson = R"JSON({"asset":{"version":"2.0"},
"buffers":[{"byteLength":36}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
"meshes":[{"name":"PropMesh","primitives":[{"attributes":{"POSITION":0},"mode":4}]}],
"nodes":[{"name":"Prop","mesh":0}],
"scenes":[{"nodes":[0]}],"scene":0})JSON";

std::vector<u8> static_bin() {
    std::vector<u8> b;
    push_f32(b, 0.0f); push_f32(b, 0.0f); push_f32(b, 0.0f);
    push_f32(b, 1.0f); push_f32(b, 0.0f); push_f32(b, 0.0f);
    push_f32(b, 0.0f); push_f32(b, 1.0f); push_f32(b, 0.0f);
    return b;
}

// ---------------------------------------------------------------------------
// sandbox + process helpers
// ---------------------------------------------------------------------------

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_glb(const std::filesystem::path& p, const std::string& json,
               const std::vector<u8>& bin) {
    const std::vector<u8> bytes = make_glb(json, bin);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// A scratch directory outside the repository. Nothing here is left behind.
struct CliSandbox {
    std::filesystem::path root;
    bool ok = false;

    explicit CliSandbox(const std::string& name) {
        std::error_code ec;
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        ok = !ec;
    }
    ~CliSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    CliSandbox(const CliSandbox&) = delete;
    CliSandbox& operator=(const CliSandbox&) = delete;
};

// Sorted relative paths of every regular file under `dir`.
std::vector<std::string> listing(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) {
            out.push_back(std::filesystem::relative(it->path(), dir, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct RunResult {
    int exit_code = -1;
    std::string log;
};

RunResult run_importer(const std::filesystem::path& scratch, const std::string& args) {
    const std::string exe = std::string(NF_MODEL_IMPORTER_EXE);
    const auto log_path = scratch / "_importer_cli.log";
    // The command must NOT begin with a quote: cmd.exe's /c quoting heuristic
    // strips the first and last quote when a line holds more than two, which
    // splits this repo's "NOVAForge Engine" path at the space. Leading with
    // `cd` sidesteps the heuristic entirely; every path passed in `args` is
    // absolute, so the working directory does not matter.
    const std::string cmd = "cd /d \"" + scratch.string() + "\" && \"" + exe + "\" " + args +
                            " > \"" + log_path.string() + "\" 2>&1";
    RunResult r;
    r.exit_code = std::system(cmd.c_str());
    r.log = read_text(log_path);
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
    return r;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string quoted(const std::filesystem::path& p) { return "\"" + p.string() + "\""; }

} // namespace

// ---------------------------------------------------------------------------
// 1. the acceptance report: a skinned character says everything it imported
// ---------------------------------------------------------------------------

NF_TEST(model_importer_info_reports_a_skinned_character) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_info");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Hero.glb";
    if (!write_glb(glb, kCharacterJson, character_bin())) NF_SKIP("cannot write fixture");

    const RunResult r = run_importer(sb.root, "--input " + quoted(glb) + " --info");
    NF_CHECK(r.exit_code == 0);

    // Geometry, material, and the skin binding — reported, not assumed.
    NF_CHECK(contains(r.log, "meshes: 1"));
    NF_CHECK(contains(r.log, "materials: 1"));
    NF_CHECK(contains(r.log, "HeroMat"));
    NF_CHECK(contains(r.log, "skin: skin 0, 3 bound verts"));

    // Skeleton: one joint, named, with its root.
    NF_CHECK(contains(r.log, "skins: 1"));
    NF_CHECK(contains(r.log, "HeroRig"));
    NF_CHECK(contains(r.log, "1 joint(s)"));
    NF_CHECK(contains(r.log, "joint 0 -> node 1 'Bone0'"));

    // Clips: both sampled ones present, the unsupported one counted.
    NF_CHECK(contains(r.log, "animations: 3"));
    NF_CHECK(contains(r.log, "Idle"));
    NF_CHECK(contains(r.log, "Wave"));
    NF_CHECK(contains(r.log, "Splined"));

    // The no-silent-substitution ledger.
    NF_CHECK(contains(r.log, "primitives (non-triangle / unusable): 0"));
    NF_CHECK(contains(r.log, "animation channels (STEP / CUBICSPLINE / morph / unsupported): 1"));
    NF_CHECK(contains(r.log, "skin bindings rejected (malformed JOINTS_0/WEIGHTS_0): 0"));
}

// ---------------------------------------------------------------------------
// 2. --info is a report, not a cook
// ---------------------------------------------------------------------------

NF_TEST(model_importer_info_writes_nothing_to_disk) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_readonly");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Hero.glb";
    if (!write_glb(glb, kCharacterJson, character_bin())) NF_SKIP("cannot write fixture");

    const std::vector<std::string> before = listing(sb.root);
    const RunResult r = run_importer(sb.root, "--input " + quoted(glb) + " --info");
    NF_CHECK(r.exit_code == 0);
    const std::vector<std::string> after = listing(sb.root);
    NF_CHECK(before == after);
    NF_CHECK(after.size() == 1); // the fixture itself, nothing else
}

// ---------------------------------------------------------------------------
// 3. character sources are never cooked as geometry-only static meshes
// ---------------------------------------------------------------------------

NF_TEST(model_importer_refuses_character_geometry_cook) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_character_refusal");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Hero.glb";
    const auto nfmesh = sb.root / "Hero.nfmesh";
    if (!write_glb(glb, kCharacterJson, character_bin())) NF_SKIP("cannot write fixture");

    const RunResult r =
        run_importer(sb.root, "--input " + quoted(glb) + " --output " + quoted(nfmesh));
    NF_CHECK(r.exit_code == 3);
    NF_CHECK(contains(r.log, "refusing geometry-only cook"));
    NF_CHECK(contains(r.log, "no output was written"));
    NF_CHECK(!std::filesystem::exists(nfmesh));
}

// ---------------------------------------------------------------------------
// 4. strict mode validates a complete character but writes no fake cook
// ---------------------------------------------------------------------------

NF_TEST(model_importer_strict_character_validation_is_read_only) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_strict_character");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Hero.glb";
    if (!write_glb(glb, kStrictCharacterJson, character_bin())) NF_SKIP("cannot write fixture");

    const std::vector<std::string> before = listing(sb.root);
    const RunResult r = run_importer(sb.root, "--input " + quoted(glb) + " --character");
    NF_CHECK(r.exit_code == 0);
    NF_CHECK(contains(r.log, "strict character validation passed"));
    NF_CHECK(contains(r.log, "no files written (validation-only mode)"));
    NF_CHECK(listing(sb.root) == before);
}

NF_TEST(model_importer_strict_character_rejects_static_prop_with_ledger) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_strict_static");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Prop.glb";
    if (!write_glb(glb, kStaticJson, static_bin())) NF_SKIP("cannot write fixture");

    const RunResult r = run_importer(sb.root, "--input " + quoted(glb) + " --character");
    NF_CHECK(r.exit_code == 1);
    NF_CHECK(contains(r.log, "skins: 0"));
    NF_CHECK(contains(r.log, "animations: 0"));
    NF_CHECK(contains(r.log, "strict character validation failed"));
    NF_CHECK(listing(sb.root).size() == 1);
}

// ---------------------------------------------------------------------------
// 5. a static source keeps the non-strict geometry cook
// ---------------------------------------------------------------------------

NF_TEST(model_importer_non_strict_static_prop_cooks_geometry) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_static_cook");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Prop.glb";
    const auto nfmesh = sb.root / "Prop.nfmesh";
    if (!write_glb(glb, kStaticJson, static_bin())) NF_SKIP("cannot write fixture");

    const RunResult r =
        run_importer(sb.root, "--input " + quoted(glb) + " --output " + quoted(nfmesh));
    NF_CHECK(r.exit_code == 0);
    NF_CHECK(contains(r.log, "writing geometry-only .nfmesh for a static source"));
    NF_CHECK(contains(r.log, "wrote"));
    NF_CHECK(std::filesystem::exists(nfmesh));

    std::string err;
    std::unique_ptr<assets::MeshAsset> mesh = assets::MeshAsset::load_from_file(nfmesh.string(), err);
    NF_CHECK(mesh != nullptr);
    if (mesh) {
        NF_CHECK(mesh->vertices.size() == 3);
        NF_CHECK(mesh->indices.size() == 3);
    }
}

// ---------------------------------------------------------------------------
// 6. extension/content mismatches are never interpreted by content alone
// ---------------------------------------------------------------------------

NF_TEST(model_importer_rejects_extension_content_mismatch) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_extension_mismatch");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto misnamed_glb = sb.root / "Hero.gltf";
    const auto nfmesh = sb.root / "Hero.nfmesh";
    if (!write_glb(misnamed_glb, kStrictCharacterJson, character_bin())) {
        NF_SKIP("cannot write fixture");
    }

    const RunResult r = run_importer(
        sb.root, "--input " + quoted(misnamed_glb) + " --output " + quoted(nfmesh));
    NF_CHECK(r.exit_code == 1);
    NF_CHECK(contains(r.log, "extension/content mismatch"));
    NF_CHECK(!std::filesystem::exists(nfmesh));
}

// ---------------------------------------------------------------------------
// 7. a prop with no rig says so; it is never given a fake skeleton
// ---------------------------------------------------------------------------

NF_TEST(model_importer_info_calls_a_static_prop_static) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_static");
    if (!sb.ok) NF_SKIP("no scratch directory");
    const auto glb = sb.root / "Prop.glb";
    if (!write_glb(glb, kStaticJson, static_bin())) NF_SKIP("cannot write fixture");

    const RunResult r = run_importer(sb.root, "--input " + quoted(glb) + " --info");
    NF_CHECK(r.exit_code == 0);
    NF_CHECK(contains(r.log, "meshes: 1"));
    NF_CHECK(contains(r.log, "skins: 0"));
    NF_CHECK(contains(r.log, "animations: 0"));
    NF_CHECK(contains(r.log, "static mesh (skin_index = -1)"));
}

// ---------------------------------------------------------------------------
// 8. bad invocation is refused, not guessed at
// ---------------------------------------------------------------------------

NF_TEST(model_importer_refuses_an_incomplete_invocation) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_usage");
    if (!sb.ok) NF_SKIP("no scratch directory");

    // No arguments at all.
    const RunResult none = run_importer(sb.root, "");
    NF_CHECK(none.exit_code == 2);
    NF_CHECK(contains(none.log, "Usage:"));

    // An input with nowhere to write and no report requested.
    const auto glb = sb.root / "Hero.glb";
    if (!write_glb(glb, kCharacterJson, character_bin())) NF_SKIP("cannot write fixture");
    const RunResult half = run_importer(sb.root, "--input " + quoted(glb));
    NF_CHECK(half.exit_code == 2);
    NF_CHECK(contains(half.log, "Usage:"));

    // An input that does not exist fails with a reason, not a crash.
    const RunResult missing =
        run_importer(sb.root, "--input " + quoted(sb.root / "nope.glb") + " --info");
    NF_CHECK(missing.exit_code == 1);
    NF_CHECK(contains(missing.log, "import failed"));
}

// ---------------------------------------------------------------------------
// 9. the CLI reads every format the engine's mesh reader does
//
// Before MeshImport existed the CLI was glTF-only and the engine could write
// six formats it could not read back. This pins the other direction of that
// fix: the shipped tool accepts an OBJ, reports what it is, and cooks it.
// ---------------------------------------------------------------------------

NF_TEST(model_importer_accepts_obj_and_cooks_it) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_obj");
    if (!sb.ok) NF_SKIP("no scratch directory");

    const auto obj = sb.root / "Tri.obj";
    {
        const std::string text =
            "# hand-written\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 0 1\n"
            "vn 0 0 1\n"
            "f 1/1/1 2/2/1 3/3/1\n";
        std::ofstream out(obj, std::ios::binary | std::ios::trunc);
        if (!out) NF_SKIP("cannot write fixture");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    // --info names the format it read, and stays read-only.
    const RunResult info = run_importer(sb.root, "--input " + quoted(obj) + " --info");
    NF_CHECK(info.exit_code == 0);
    NF_CHECK(contains(info.log, "format: Wavefront OBJ"));
    NF_CHECK(contains(info.log, "meshes: 1"));
    NF_CHECK(contains(info.log, "static mesh (skin_index = -1)"));
    NF_CHECK(contains(info.log, "warnings: 0"));
    NF_CHECK(listing(sb.root).size() == 1); // the fixture, nothing else

    // The cook writes a real .nfmesh the engine can load.
    const auto nfmesh = sb.root / "Tri.nfmesh";
    const RunResult cook =
        run_importer(sb.root, "--input " + quoted(obj) + " --output " + quoted(nfmesh));
    NF_CHECK(cook.exit_code == 0);
    NF_CHECK(contains(cook.log, "writing geometry-only .nfmesh for a static source"));
    NF_CHECK(contains(cook.log, "wrote"));
    NF_CHECK(std::filesystem::exists(nfmesh));

    std::string err;
    std::unique_ptr<assets::MeshAsset> mesh = assets::MeshAsset::load_from_file(nfmesh.string(), err);
    NF_CHECK(mesh != nullptr);
    if (mesh) {
        NF_CHECK(mesh->vertices.size() == 3);
        NF_CHECK(mesh->indices.size() == 3);
        // The normals came from the file, so they are the file's +Z.
        NF_CHECK_NEAR(mesh->vertices[0].normal[2], 1.0f, 1e-5f);
    }
}

NF_TEST(model_importer_accepts_stl_and_reports_its_losses) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_stl");
    if (!sb.ok) NF_SKIP("no scratch directory");

    const auto stl = sb.root / "Tri.stl";
    {
        const std::string text =
            "solid tri\n"
            "  facet normal 0 0 1\n"
            "    outer loop\n"
            "      vertex 0 0 0\n"
            "      vertex 1 0 0\n"
            "      vertex 0 1 0\n"
            "    endloop\n"
            "  endfacet\n"
            "endsolid tri\n";
        std::ofstream out(stl, std::ios::binary | std::ios::trunc);
        if (!out) NF_SKIP("cannot write fixture");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    const RunResult r = run_importer(sb.root, "--input " + quoted(stl) + " --info");
    NF_CHECK(r.exit_code == 0);
    NF_CHECK(contains(r.log, "format: STL (binary or ASCII)"));
    // STL's losses are named, not implied: no UVs, no vertex normals.
    NF_CHECK(contains(r.log, "warnings: 1"));
    NF_CHECK(contains(r.log, "no UVs"));
}

// ---------------------------------------------------------------------------
// 10. materials and textures are reported, and their loss is named
// ---------------------------------------------------------------------------

NF_TEST(model_importer_reports_obj_materials_and_texture_loss) {
    if (std::string(NF_MODEL_IMPORTER_EXE).empty()) NF_SKIP("tools not configured");
    CliSandbox sb("nf_importer_cli_obj_material");
    if (!sb.ok) NF_SKIP("no scratch directory");

    const auto obj = sb.root / "Panel.obj";
    const auto mtl = sb.root / "Panel.mtl";
    const auto png = sb.root / "wood.png";
    {
        const std::string obj_text =
            "mtllib Panel.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 0 1\n"
            "vn 0 0 1\n"
            "usemtl wood\n"
            "f 1/1/1 2/2/1 3/3/1\n";
        std::ofstream out(obj, std::ios::binary | std::ios::trunc);
        if (!out) NF_SKIP("cannot write fixture");
        out.write(obj_text.data(), static_cast<std::streamsize>(obj_text.size()));
    }
    {
        const std::string mtl_text = "newmtl wood\nKd 0.55 0.35 0.15\nKs 1 1 1\nNs 32\n"
                                     "map_Kd wood.png\n";
        std::ofstream out(mtl, std::ios::binary | std::ios::trunc);
        if (!out) NF_SKIP("cannot write fixture");
        out.write(mtl_text.data(), static_cast<std::streamsize>(mtl_text.size()));
    }
    {
        const std::vector<u8> png_bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                           0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
        std::ofstream out(png, std::ios::binary | std::ios::trunc);
        if (!out) NF_SKIP("cannot write fixture");
        out.write(reinterpret_cast<const char*>(png_bytes.data()),
                  static_cast<std::streamsize>(png_bytes.size()));
    }

    // --info: the material, its factors, its texture, and the fields the
    // pipeline has no room for — all named.
    const RunResult info = run_importer(sb.root, "--input " + quoted(obj) + " --info");
    NF_CHECK(info.exit_code == 0);
    NF_CHECK(contains(info.log, "materials: 1"));
    NF_CHECK(contains(info.log, "wood"));
    NF_CHECK(contains(info.log, "albedo image 0"));
    NF_CHECK(contains(info.log, "images: 1"));
    NF_CHECK(contains(info.log, "wood.png"));
    NF_CHECK(contains(info.log, "dropped: Ks "));
    NF_CHECK(contains(info.log, "dropped: Ns "));

    // The cook writes geometry only, and says exactly what it could not carry
    // rather than leaving a grey model and no explanation.
    const auto nfmesh = sb.root / "Panel.nfmesh";
    const RunResult cook =
        run_importer(sb.root, "--input " + quoted(obj) + " --output " + quoted(nfmesh));
    NF_CHECK(cook.exit_code == 0);
    NF_CHECK(contains(cook.log, "1 material(s) and 1 image(s) are NOT written"));
    NF_CHECK(std::filesystem::exists(nfmesh));
}
