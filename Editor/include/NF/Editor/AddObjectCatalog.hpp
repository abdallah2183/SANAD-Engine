#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace nf::editor {

enum class AddObjectKind { Empty, Cube, Sphere, Quad, Ground, Sprite, Tilemap, Camera,
                           Sun, PointLight, SpotLight, Audio, Sky, Weather, Cable,
                           RuleTile, FlowField, Talent };
enum class AddObjectGroup { Basic, TwoD, Light, World, Advanced };
struct AddObjectEntry {
    AddObjectKind kind;
    AddObjectGroup group;
    const char* label_key;
    const char* description_key;
    const char* english_search;
};
inline constexpr std::array<AddObjectEntry, 18> kAddObjects{{
    {AddObjectKind::Empty, AddObjectGroup::Basic, "palette_empty", "palette_desc_empty", "empty node entity object"},
    {AddObjectKind::Cube, AddObjectGroup::Basic, "cube", "palette_desc_cube", "cube box mesh 3d"},
    {AddObjectKind::Sphere, AddObjectGroup::Basic, "sphere", "palette_desc_sphere", "sphere ball mesh 3d"},
    {AddObjectKind::Quad, AddObjectGroup::Basic, "quad", "palette_desc_quad", "quad plane mesh 3d"},
    {AddObjectKind::Ground, AddObjectGroup::Basic, "ground", "palette_desc_ground", "ground floor plane"},
    {AddObjectKind::Sprite, AddObjectGroup::TwoD, "sprite_2d", "palette_desc_sprite", "sprite 2d image"},
    {AddObjectKind::Tilemap, AddObjectGroup::TwoD, "tilemap_2d", "palette_desc_tilemap", "tilemap tiles 2d"},
    {AddObjectKind::Camera, AddObjectGroup::Light, "camera", "palette_desc_camera", "camera view"},
    {AddObjectKind::Sun, AddObjectGroup::Light, "light_sun", "palette_desc_sun", "directional sun light"},
    {AddObjectKind::PointLight, AddObjectGroup::Light, "point_light", "palette_desc_point", "point light lamp"},
    {AddObjectKind::SpotLight, AddObjectGroup::Light, "spot_light", "palette_desc_spot", "spot light flashlight"},
    {AddObjectKind::Audio, AddObjectGroup::World, "audio_source", "palette_desc_audio", "audio sound source"},
    {AddObjectKind::Sky, AddObjectGroup::World, "sky", "palette_desc_sky", "sky environment"},
    {AddObjectKind::Weather, AddObjectGroup::World, "add_weather", "palette_desc_weather", "weather fog rain wind"},
    {AddObjectKind::Cable, AddObjectGroup::Advanced, "add_cable", "palette_desc_cable", "cable rope"},
    {AddObjectKind::RuleTile, AddObjectGroup::Advanced, "add_rule_tile", "palette_desc_rule", "rule tile"},
    {AddObjectKind::FlowField, AddObjectGroup::Advanced, "add_flow_field", "palette_desc_flow", "flow field navigation"},
    {AddObjectKind::Talent, AddObjectGroup::Advanced, "add_talent_tree", "palette_desc_talent", "talent progression"},
}};
inline std::string add_object_fold(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 128) c = static_cast<char>(std::tolower(b));
    }
    return out;
}
inline bool add_object_matches(const AddObjectEntry& entry, std::string_view localized_label,
                               std::string_view query, std::string_view alternate_label = {}) {
    const auto q = add_object_fold(query);
    const auto label = add_object_fold(localized_label);
    const auto alternate = add_object_fold(alternate_label);
    const auto keywords = add_object_fold(entry.english_search);
    // Treat separate words as independent terms. This supports natural queries
    // such as "3D cube" and ignores accidental leading or trailing spaces.
    size_t pos = 0;
    while (pos < q.size()) {
        while (pos < q.size() && std::isspace(static_cast<unsigned char>(q[pos]))) ++pos;
        const size_t start = pos;
        while (pos < q.size() && !std::isspace(static_cast<unsigned char>(q[pos]))) ++pos;
        if (start == pos) break;
        const auto term = q.substr(start, pos - start);
        if (label.find(term) == std::string::npos && alternate.find(term) == std::string::npos &&
            keywords.find(term) == std::string::npos)
            return false;
    }
    return true;
}
} // namespace nf::editor
