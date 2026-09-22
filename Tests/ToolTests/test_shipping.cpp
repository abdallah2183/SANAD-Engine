// Tests/ToolTests/test_shipping.cpp — `nf package --shipping`
//
// The packager tests next door pin the *folder*: it is self-contained, it is
// relocatable, a broken asset stops the build. This file pins the half that
// turns that folder into something a stranger can receive, evaluate, run, and
// remove:
//
//   1. the shipping extras are staged (VERSION.txt stamp, README, REDIST,
//      UNINSTALL) and a version string is mandatory for a shipping build;
//   2. the archive is a real, readable zip whose CRCs match its bytes;
//   3. the packaged player actually RUNS from a relocated folder with no
//      arguments at all — the "package from machine A runs elsewhere" claim,
//      proven as far as a single machine can prove it;
//   4. the crash path writes its minidump and its human-readable report INSIDE
//      the package, which is what makes "delete the folder" a complete
//      uninstall.
//
// (3) and (4) launch the packaged NFPlayer.exe as a subprocess, so they exercise
// the shipping artifact rather than a test-only harness.

#include <NF/Test/TestFramework.hpp>
#include <NF/Project/ProjectPackager.hpp>
#include <NF/Project/ProjectScaffold.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace nf;
using namespace nf::project;

#ifndef NF_TEMPLATE_DIR
    #define NF_TEMPLATE_DIR ""
#endif
#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif
#ifndef NF_PLAYER_EXE
    #define NF_PLAYER_EXE ""
#endif

namespace {

// --- small IO helpers -------------------------------------------------------

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- zip reader -------------------------------------------------------------
//
// Deliberately independent of the writer: it goes through the end-of-central-
// directory record, then the central directory, then each local file header,
// exactly as a consumer's unzip does. A writer bug that only "looks right" to
// its own reader is the failure this is here to catch.

uint16_t le16(const std::vector<uint8_t>& b, size_t o) {
    return static_cast<uint16_t>(b[o] | (static_cast<uint16_t>(b[o + 1]) << 8));
}

uint32_t le32(const std::vector<uint8_t>& b, size_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}

struct ZipEntry {
    std::string name;
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

std::vector<ZipEntry> parse_zip(const std::vector<uint8_t>& b) {
    std::vector<ZipEntry> entries;
    if (b.size() < 22) return entries;

    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 21; i-- > 0;) {
        if (le32(b, i) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return entries;

    const uint16_t count = le16(b, eocd + 10);
    const uint32_t cd_off = le32(b, eocd + 16);
    size_t p = cd_off;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > b.size() || le32(b, p) != 0x02014B50u) return {};
        ZipEntry e;
        e.crc = le32(b, p + 16);
        e.size = le32(b, p + 24);
        const uint16_t name_len = le16(b, p + 28);
        const uint16_t extra_len = le16(b, p + 30);
        const uint16_t comment_len = le16(b, p + 32);
        e.offset = le32(b, p + 42);
        if (p + 46 + name_len > b.size()) return {};
        e.name.assign(reinterpret_cast<const char*>(b.data() + p + 46), name_len);
        entries.push_back(std::move(e));
        p += 46u + name_len + extra_len + comment_len;
    }
    return entries;
}

std::vector<uint8_t> zip_entry_bytes(const std::vector<uint8_t>& b, const ZipEntry& e) {
    if (static_cast<size_t>(e.offset) + 30 > b.size() || le32(b, e.offset) != 0x04034B50u) {
        return {};
    }
    const uint16_t name_len = le16(b, e.offset + 26);
    const uint16_t extra_len = le16(b, e.offset + 28);
    const size_t start = static_cast<size_t>(e.offset) + 30 + name_len + extra_len;
    if (start + e.size > b.size()) return {};
    return std::vector<uint8_t>(b.begin() + static_cast<std::ptrdiff_t>(start),
                                b.begin() + static_cast<std::ptrdiff_t>(start + e.size));
}

// --- sandbox ----------------------------------------------------------------

struct ShipSandbox {
    std::filesystem::path root;
    std::filesystem::path project_file;
    bool ok = false;

    explicit ShipSandbox(const std::string& name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        ScaffoldOptions o;
        o.root = root;
        o.name = "Demo";
        o.template_dir = std::string(NF_TEMPLATE_DIR);
        std::string err;
        ok = scaffold_project(o, err);
        project_file = root / "Demo.nfproj";
    }
    ~ShipSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // `with_player` is opt-in: the player is a ~14 MB debug binary, and only the
    // tests that launch it need it in the package. Keeping it out of the others
    // keeps this suite cheap for the ten models that run it concurrently.
    BuildOptions options(const std::string& out_dir = {}, bool with_player = false) const {
        BuildOptions o;
        o.project_file = project_file;
        o.shader_dir = std::string(NF_BASIC3D_SHADER_DIR);
        if (with_player) o.player_exe = std::string(NF_PLAYER_EXE);
        if (!out_dir.empty()) o.output_dir = out_dir;
        return o;
    }

    std::filesystem::path dist() const { return root / "dist"; }
};

// Every regular file under `dir`, relative to `dir`, sorted. Used to compare a
// package's contents before and after a run.
std::vector<std::string> tree_listing(const std::filesystem::path& dir) {
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

// Run `exe` with its working directory set to `dir`. The log is written OUTSIDE
// the package so a test can still assert the package tree changed by exactly
// the files the game itself created.
RunResult run_in_dir(const std::filesystem::path& dir,
                     const std::string& exe,
                     const std::string& args) {
#if defined(_WIN32)
    // The child is (in one case) a deliberately crashing process. Child
    // processes inherit the parent's error mode, so suppressing the WER fault
    // box here means the suite can never block on a modal dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    const auto log_path = dir.parent_path() / (dir.filename().string() + "_run.log");
    std::string cmd = "cd /d \"" + dir.string() + "\" && \"" + exe + "\" " + args +
                      " > \"" + log_path.string() + "\" 2>&1";
    RunResult r;
    r.exit_code = std::system(cmd.c_str());
    r.log = read_text(log_path);
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
    return r;
}

// The player the shipping package carries. Empty when the tools were configured
// out of the build.
std::string player_exe() { return std::string(NF_PLAYER_EXE); }

bool shipping_prereqs_ok() {
    return !std::string(NF_TEMPLATE_DIR).empty() && !std::string(NF_BASIC3D_SHADER_DIR).empty();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. the shipping extras
// ---------------------------------------------------------------------------

NF_TEST(shipping_package_stages_a_version_stamp_and_the_notes) {
    ShipSandbox sb("nf_ship_stamp");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    auto opts = sb.options();
    opts.shipping = true;
    opts.version = "nf 9.9.9-test (G6)";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(opts, report, err));

    // The stamp is the thing that makes an incoming crash report triageable, so
    // every field in it is load-bearing.
    const std::string version = read_text(sb.dist() / "VERSION.txt");
    NF_CHECK(!version.empty());
    NF_CHECK(contains(version, "project:    Demo"));
    NF_CHECK(contains(version, "tool:       nf 9.9.9-test (G6)"));
    NF_CHECK(contains(version, "target:     windows-x64"));
    // `built:` must be an ISO 8601 UTC stamp — 20 chars, 'T' at 10, 'Z' at 19.
    const auto built = version.find("built:");
    NF_CHECK(built != std::string::npos);
    size_t s = built + 6; // past "built:"
    while (s < version.size() && version[s] == ' ') ++s;
    const std::string stamp = version.substr(s, 20);
    NF_CHECK_EQ(stamp.size(), size_t{20});
    NF_CHECK_EQ(stamp[10], 'T');
    NF_CHECK_EQ(stamp[19], 'Z');
    for (size_t i = 0; i < 20; ++i) {
        if (i == 10 || i == 19) continue;
        if (i == 13 || i == 16) { NF_CHECK_EQ(stamp[i], ':'); continue; }
        if (i == 4 || i == 7) { NF_CHECK_EQ(stamp[i], '-'); continue; }
        NF_CHECK(stamp[i] >= '0' && stamp[i] <= '9');
    }

    // README: how to run, and where crashes land.
    const std::string readme = read_text(sb.dist() / "README.txt");
    NF_CHECK(contains(readme, "NFPlayer.exe"));
    NF_CHECK(contains(readme, "Demo.nfproj"));
    NF_CHECK(contains(readme, "Crashes/"));
    NF_CHECK(contains(readme, "UNINSTALL.txt"));

    // REDIST: the honest note about an unsigned build, and what the OS supplies.
    const std::string redist = read_text(sb.dist() / "REDIST.txt");
    NF_CHECK(contains(redist, "NOT code-signed"));
    NF_CHECK(contains(redist, "signtool"));
    NF_CHECK(contains(redist, "Vulkan"));
    NF_CHECK(contains(redist, "No Visual C++ redistributable is needed"));

    // UNINSTALL: what is written, and that deleting the folder is the whole job.
    const std::string uninstall = read_text(sb.dist() / "UNINSTALL.txt");
    NF_CHECK(contains(uninstall, "Delete this folder"));
    NF_CHECK(contains(uninstall, "Crashes/"));
    NF_CHECK(contains(uninstall, "registry entries"));
    NF_CHECK(contains(uninstall, "%APPDATA%"));
}

NF_TEST(shipping_package_refuses_to_build_without_a_version) {
    ShipSandbox sb("nf_ship_noversion");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    auto opts = sb.options();
    opts.shipping = true;
    opts.version = ""; // a distributable with no version is untriageable
    BuildReport report;
    std::string err;
    NF_CHECK(!build_project(opts, report, err));
    NF_CHECK(contains(err, "version"));
    // Nothing half-staged: a rejected shipping build must not leave a zip.
    NF_CHECK(!std::filesystem::exists(std::filesystem::path(sb.dist().string() + ".zip")));
}

NF_TEST(a_plain_build_has_no_shipping_artifacts_and_clears_a_stale_zip) {
    ShipSandbox sb("nf_ship_plain");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    const auto zip_path = std::filesystem::path(sb.dist().string() + ".zip");

    auto shipping = sb.options();
    shipping.shipping = true;
    shipping.version = "nf 9.9.9-test (G6)";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(shipping, report, err));
    NF_CHECK(std::filesystem::exists(zip_path));

    // Rebuild WITHOUT --shipping: the notes and the archive must both go, or a
    // stale distributable lingers next to a package that no longer matches it.
    BuildReport plain;
    NF_CHECK(build_project(sb.options(), plain, err));
    NF_CHECK(!std::filesystem::exists(sb.dist() / "VERSION.txt"));
    NF_CHECK(!std::filesystem::exists(sb.dist() / "README.txt"));
    NF_CHECK(!std::filesystem::exists(sb.dist() / "REDIST.txt"));
    NF_CHECK(!std::filesystem::exists(sb.dist() / "UNINSTALL.txt"));
    NF_CHECK(!std::filesystem::exists(zip_path));
    NF_CHECK(plain.zip_path.empty());
}

// ---------------------------------------------------------------------------
// 2. the archive
// ---------------------------------------------------------------------------

NF_TEST(shipping_zip_is_a_readable_archive_covering_the_whole_package) {
    ShipSandbox sb("nf_ship_zip");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    // The archive must carry the runnable game, so this one ships the player.
    auto opts = sb.options({}, true);
    opts.shipping = true;
    opts.version = "nf 9.9.9-test (G6)";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(opts, report, err));

    const auto zip_path = std::filesystem::path(sb.dist().string() + ".zip");
    NF_CHECK_EQ(report.zip_path.string(), zip_path.lexically_normal().string());
    NF_CHECK(std::filesystem::exists(zip_path));
    // Written NEXT TO the package, never inside it: re-zipping must not nest.
    NF_CHECK(!std::filesystem::exists(sb.dist() / zip_path.filename()));

    const auto bytes = read_bytes(zip_path);
    NF_CHECK(bytes.size() > 22);
    const auto entries = parse_zip(bytes);
    NF_CHECK(!entries.empty());

    const auto on_disk = tree_listing(sb.dist());
    NF_CHECK_EQ(entries.size(), on_disk.size());

    // Every file in the folder is in the archive, under its relative path, and
    // its CRC matches the bytes its local header points at.
    std::vector<std::string> names;
    for (const auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    NF_CHECK(names == on_disk);

    for (const auto& e : entries) {
        const auto data = zip_entry_bytes(bytes, e);
        NF_CHECK_EQ(data.size(), static_cast<size_t>(e.size));
        NF_CHECK_EQ(crc32_of(data.data(), data.size()), e.crc);
    }

    // Spot-check the entry a player is most likely to open first.
    NF_CHECK(std::find(names.begin(), names.end(), "VERSION.txt") != names.end());
    NF_CHECK(std::find(names.begin(), names.end(), "Demo.nfproj") != names.end());
}

// ---------------------------------------------------------------------------
// 3. the package runs — the "machine A to machine B" claim
// ---------------------------------------------------------------------------

NF_TEST(packaged_tree_runs_headless_from_a_relocated_folder) {
    if (player_exe().empty()) { NF_SKIP("NFPlayer not built (NF_BUILD_TOOLS=OFF)"); }
    ShipSandbox sb("nf_ship_run");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    auto opts = sb.options({}, true);
    opts.shipping = true;
    opts.version = "nf 9.9.9-test (G6)";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(opts, report, err));

    // Move the package somewhere else entirely: a different path proves the
    // descriptor's relative mounts resolve against the package, not the build
    // tree it came from.
    const auto moved = sb.root / "elsewhere" / "InstalledGame";
    std::filesystem::remove_all(sb.root / "elsewhere");
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::copy(sb.dist(), moved, std::filesystem::copy_options::recursive);
    NF_CHECK(std::filesystem::exists(moved / "NFPlayer.exe"));

    // No --project: the player must find the single .nfproj beside itself.
    const auto run = run_in_dir(moved, (moved / "NFPlayer.exe").string(),
                                "--frames 10 --headless");
    NF_CHECK_EQ(run.exit_code, 0);
    NF_CHECK(contains(run.log, "Runtime exited cleanly"));
    NF_CHECK(contains(run.log, "Rendered 10 frames"));
    // The runtime read its shaders and its mesh out of the package.
    NF_CHECK(contains(run.log, "InstalledGame"));
    NF_CHECK(contains(run.log, "cube.nfmesh"));
    // And it left no RHI objects behind on the way out.
    NF_CHECK(contains(run.log, "Alive RHI objects before shutdown: 0"));
}

// ---------------------------------------------------------------------------
// 4. the crash path, and why "delete the folder" is a complete uninstall
// ---------------------------------------------------------------------------

NF_TEST(packaged_player_writes_crash_artifacts_inside_its_own_folder) {
    if (player_exe().empty()) { NF_SKIP("NFPlayer not built (NF_BUILD_TOOLS=OFF)"); }
    ShipSandbox sb("nf_ship_crash");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (!shipping_prereqs_ok()) { NF_SKIP("template or shader dir not configured"); }

    auto opts = sb.options({}, true);
    opts.shipping = true;
    opts.version = "nf 9.9.9-test (G6)";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(opts, report, err));

    // Run the package where it sits: the point here is WHERE the artifacts land,
    // not relocation (that is the test above).
    const auto pkg = sb.dist();

    const auto before = tree_listing(pkg);

    // Bare --crash-test: arm the real filter, raise a real fatal exception, and
    // let the SEH path write. This is the same binary a customer runs.
    const auto run = run_in_dir(pkg, (pkg / "NFPlayer.exe").string(), "--crash-test");
    NF_CHECK(run.exit_code != 0); // a dump is not a recovery

    const auto crashes = pkg / "Crashes";
    NF_CHECK(std::filesystem::exists(crashes));

    std::vector<std::filesystem::path> dumps, reports;
    for (auto it = std::filesystem::directory_iterator(crashes); it != std::filesystem::directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() == ".dmp") dumps.push_back(it->path());
        else if (it->path().extension() == ".txt") reports.push_back(it->path());
    }
    NF_CHECK_EQ(dumps.size(), size_t{1});
    NF_CHECK_EQ(reports.size(), size_t{1});

    // A real Windows minidump, not an empty placeholder.
    const auto dump = read_bytes(dumps.front());
    NF_CHECK(dump.size() > 1024);
    NF_CHECK_EQ(std::string(reinterpret_cast<const char*>(dump.data()), 4), std::string("MDMP"));

    // The readable twin: same stem, and the fields a triager needs.
    NF_CHECK_EQ(reports.front().stem().string(), dumps.front().stem().string());
    const std::string text = read_text(reports.front());
    NF_CHECK(contains(text, "NOVAForge crash report"));
    NF_CHECK(contains(text, "exception:  0xE0000046"));
    NF_CHECK(contains(text, "version:    NFPlayer 0.1"));
    NF_CHECK(contains(text, dumps.front().filename().string()));
    NF_CHECK(contains(text, "written")); // the dump itself succeeded

    // The uninstall story: the crash artifacts are the ONLY thing the run added,
    // and they are inside the package folder — so deleting the folder removes
    // every trace of the game.
    auto after = tree_listing(pkg);
    std::vector<std::string> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    NF_CHECK_EQ(added.size(), size_t{2});
    for (const auto& a : added) {
        NF_CHECK(a.rfind("Crashes/", 0) == 0);
    }
}
