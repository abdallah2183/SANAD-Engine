// Per-project SettingsStore: isolation, atomic persistence, corrupt handling.
#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/SettingsStore.hpp>
#include <filesystem>
#include <fstream>
using namespace nf::assets;

namespace {
std::filesystem::path temp_base() {
    const auto tmp = std::filesystem::temp_directory_path() / "sanad_settings_test";
    std::filesystem::create_directories(tmp);
    return tmp;
}
} // namespace

NF_TEST(settings_store_isolates_projects) {
    const auto base = temp_base() / "isolation";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    SettingsStore a("GameA", base), b("GameB", base);
    std::string err;
    NF_CHECK(a.save({{"player_name", "Alice"}}, err));
    NF_CHECK(b.save({{"player_name", "Bob"}}, err));
    SettingsStore::Map out;
    NF_CHECK(a.load(out, err));
    NF_CHECK(out["player_name"] == "Alice");
    NF_CHECK(b.load(out, err));
    NF_CHECK(out["player_name"] == "Bob");
    NF_CHECK(a.settings_file() != b.settings_file());
    // Traversal-ish names never escape the base (single safe segment).
    SettingsStore evil("../../etc", base);
    const std::string evil_str = evil.settings_file().string();
    NF_CHECK(evil_str.find("..\\..") == std::string::npos);
    NF_CHECK(evil_str.find(base.string()) != std::string::npos);
    NF_CHECK(SettingsStore::safe_project_segment("") == "SANADProject");
}

NF_TEST(settings_store_round_trip_and_atomic_file) {
    const auto base = temp_base() / "roundtrip";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    SettingsStore st("RuntimeUiLab", base);
    SettingsStore::Map out;
    std::string err;
    // Missing file is not an error.
    NF_CHECK(st.load(out, err));
    NF_CHECK(out.empty());
    NF_CHECK(st.save({{"player_name", "سلام Player"}, {"volume", "80"}, {"show_hint", "1"}}, err));
    NF_CHECK(st.exists());
    // No .tmp file left behind.
    NF_CHECK(!std::filesystem::exists(st.settings_file().string() + ".tmp"));
    std::ifstream head(st.settings_file());
    std::string first;
    std::getline(head, first);
    NF_CHECK(first.find("version:") != std::string::npos);
    NF_CHECK(st.load(out, err));
    NF_CHECK(out["player_name"] == "سلام Player");
    NF_CHECK(out["volume"] == "80");
}

NF_TEST(settings_store_corrupt_file_never_crashes) {
    const auto base = temp_base() / "corrupt";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    SettingsStore st("Game", base);
    std::string err;
    NF_CHECK(st.save({{"a", "1"}}, err));
    // Corrupt: no version stamp.
    { std::ofstream f(st.settings_file(), std::ios::trunc); f << "not a settings file!!!\n@@@\n"; }
    SettingsStore::Map out = {{"keep", "me"}};
    NF_CHECK(!st.load(out, err));
    NF_CHECK(!err.empty());
    // Out map untouched on failure (caller's defaults survive).
    NF_CHECK(out["keep"] == "me");
    // Oversized file refused.
    { std::ofstream f(st.settings_file(), std::ios::trunc | std::ios::binary);
      f << "version: 1\n";
      f << std::string(SettingsStore::kMaxFileBytes + 1, 'x'); }
    NF_CHECK(!st.load(out, err));
    // Future version refused.
    { std::ofstream f(st.settings_file(), std::ios::trunc); f << "version: 99\nk = v\n"; }
    NF_CHECK(!st.load(out, err));
}

NF_TEST(settings_store_rejects_bad_keys_and_oversize) {
    const auto base = temp_base() / "limits";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    SettingsStore st("Game", base);
    std::string err;
    NF_CHECK(!st.save({{"../escape", "x"}}, err));
    NF_CHECK(!st.save({{"", "x"}}, err));
    NF_CHECK(!st.save({{"version", "1"}}, err));
    NF_CHECK(!st.save({{"k", std::string(4097, 'x')}}, err));
    NF_CHECK(!st.save({{"k", "line1\nline2"}}, err));
    NF_CHECK(SettingsStore::valid_key("player_name_1.2-3"));
    NF_CHECK(!SettingsStore::valid_key("has space"));
}
