#include <NF/Test/TestFramework.hpp>
#include <NF/Core/StringId.hpp>
#include <unordered_map>

using namespace nf;

NF_TEST(string_id_compile_time_literal) {
    constexpr StringId id1 = "Player.Health"_sid;
    constexpr StringId id2 = "Player.Health"_sid;
    constexpr StringId id3 = "Player.Mana"_sid;

    NF_CHECK(id1.valid());
    NF_CHECK(id1 == id2);
    NF_CHECK(id1 != id3);
    NF_CHECK(id1.hash() != 0);
    NF_CHECK(id1.hash() != id3.hash());
}

NF_TEST(string_id_runtime_interning) {
    StringId id_runtime("TransformComponent");
    StringId id_literal = "TransformComponent"_sid;

    NF_CHECK_EQ(id_runtime.hash(), id_literal.hash());
    NF_CHECK(id_runtime == id_literal);

    // Verify interning retrieves original string
    std::string_view sv = id_runtime.str();
    NF_CHECK_EQ(sv, "TransformComponent");
    NF_CHECK_EQ(std::string(id_runtime.c_str()), "TransformComponent");
}

NF_TEST(string_id_hash_map) {
    std::unordered_map<StringId, int> map;
    map["Player.Health"_sid] = 100;
    map["Player.Speed"_sid] = 250;

    NF_CHECK_EQ(map["Player.Health"_sid], 100);
    NF_CHECK_EQ(map["Player.Speed"_sid], 250);

    StringId lookup("Player.Health");
    NF_CHECK(map.find(lookup) != map.end());
    NF_CHECK_EQ(map[lookup], 100);
}

NF_TEST(string_utils_trim_and_casing) {
    std::string untrimmed = "  \t Hello Engine World!  \r\n ";
    std::string trimmed = strings::trim(untrimmed);
    NF_CHECK_EQ(trimmed, "Hello Engine World!");

    std::string lower = strings::to_lower("SaNaD-EnGiNe_v0.7.0");
    NF_CHECK_EQ(lower, "sanad-engine_v0.7.0");

    std::string upper = strings::to_upper("sanad");
    NF_CHECK_EQ(upper, "SANAD");
}

NF_TEST(string_utils_split_and_join) {
    std::string path = "Engine/Core/src/StringId.cpp";
    auto parts = strings::split(path, '/');
    NF_CHECK_EQ(parts.size(), 4u);
    NF_CHECK_EQ(parts[0], "Engine");
    NF_CHECK_EQ(parts[1], "Core");
    NF_CHECK_EQ(parts[2], "src");
    NF_CHECK_EQ(parts[3], "StringId.cpp");

    std::string rejoined = strings::join(parts, "::");
    NF_CHECK_EQ(rejoined, "Engine::Core::src::StringId.cpp");
}

NF_TEST(string_utils_prefix_suffix_replace) {
    std::string filename = "asset_model_mesh.mesh";
    NF_CHECK(strings::starts_with(filename, "asset_"));
    NF_CHECK(!strings::starts_with(filename, "mesh_"));
    NF_CHECK(strings::ends_with(filename, ".mesh"));
    NF_CHECK(!strings::ends_with(filename, ".png"));

    std::string replaced = strings::replace_all("apple,banana,apple,orange", "apple", "grape");
    NF_CHECK_EQ(replaced, "grape,banana,grape,orange");
}
