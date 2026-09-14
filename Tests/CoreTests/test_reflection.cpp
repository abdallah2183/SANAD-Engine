// Tests/CoreTests/test_reflection.cpp — Phase 10, W1 (reflection metadata)
//
// These tests are about the metadata being *true*, not merely present. A
// reflection system that reports a property at the wrong offset compiles, links,
// and silently corrupts data — so the offset assertions below compare against
// offsetof directly rather than against a hardcoded number.

#include <NF/Test/TestFramework.hpp>

#include <NF/Core/Math.hpp>
#include <NF/Core/Reflection.hpp>

#include <string>

using namespace nf;

namespace {

enum class DriveMode : u8 {
    Idle    = 0,
    Forward = 1,
    Reverse = 2,
};

// Deliberately signed and with a negative variant: the enum marshaller reads the
// member's raw bytes and must agree with the variant table for both signs.
enum class ErrorCode : i32 {
    None    = 0,
    Warning = -3,
    Fatal   = 1000,
};

struct BaseStats {
    f32 health = 100.0f;
    i32 lives  = 3;

    NF_CLASS(BaseStats)
    NF_PROPERTY(BaseStats, health, Float, Prop_EditAnywhere, "Stats")
    NF_PROPERTY(BaseStats, lives,  Int,   Prop_EditAnywhere | Prop_SerializeField, "Stats")
    NF_CLASS_END(BaseStats)
};

struct PlayerStats {
    f32         speed  = 7.5f;
    Vec3        spawn  = Vec3{1.0f, 2.0f, 3.0f};
    Quat        facing = Quat{0.0f, 0.0f, 0.0f, 1.0f};
    std::string name   = "hero";
    bool        alive  = true;
    DriveMode   mode   = DriveMode::Idle;
    ErrorCode   error  = ErrorCode::None;
    u32         target = u32_max;

    NF_CLASS(PlayerStats)
    NF_PROPERTY(PlayerStats, speed,  Float, Prop_EditAnywhere, "Movement")
    NF_PROPERTY(PlayerStats, spawn,  Vec3,  Prop_EditAnywhere, "Movement")
    NF_PROPERTY(PlayerStats, facing, Quat,  Prop_EditAnywhere, "Movement")
    NF_PROPERTY(PlayerStats, name,   String, Prop_EditAnywhere, "Identity")
    NF_PROPERTY(PlayerStats, alive,  Bool,  Prop_EditAnywhere, "Identity")
    NF_PROPERTY_ENUM(PlayerStats, mode,  Enum, Prop_EditAnywhere, "Movement", "DriveMode")
    NF_PROPERTY_ENUM(PlayerStats, error, Enum, Prop_EditAnywhere, "Diagnostics", "ErrorCode")
    NF_PROPERTY(PlayerStats, target, EntityRef, Prop_EditAnywhere, "Links")
    NF_CLASS_END_DERIVED(PlayerStats, BaseStats)
};

} // namespace

NF_ENUM_BEGIN(DriveMode)
    NF_ENUM_VALUE(DriveMode, Idle)
    NF_ENUM_VALUE(DriveMode, Forward)
    NF_ENUM_VALUE(DriveMode, Reverse)
NF_ENUM_END(DriveMode, "DriveMode")

NF_ENUM_BEGIN(ErrorCode)
    NF_ENUM_VALUE(ErrorCode, None)
    NF_ENUM_VALUE(ErrorCode, Warning)
    NF_ENUM_VALUE(ErrorCode, Fatal)
NF_ENUM_END(ErrorCode, "ErrorCode")

// ---------------------------------------------------------------------------

NF_TEST(reflection_class_is_registered_without_instantiation) {
    const ClassInfo* info = ReflectionRegistry::instance().find_class("PlayerStats");
    NF_CHECK(info != nullptr);
    NF_CHECK_EQ(std::string(info->name), std::string("PlayerStats"));
    NF_CHECK(info->parent != nullptr);
    NF_CHECK_EQ(std::string(info->parent->name), std::string("BaseStats"));
}

NF_TEST(reflection_property_offsets_match_offsetof) {
    PlayerStats stats;
    const ClassInfo* info = PlayerStats::nf_class_meta();
    NF_CHECK(info != nullptr);

    // The reflected offset must address the member it names. Writing through the
    // property and reading the member back is the only check that actually
    // proves this — an offset table that is merely self-consistent would pass a
    // comparison against itself.
    const PropertyInfo* speed = info->find_property("speed");
    NF_CHECK(speed != nullptr);
    NF_CHECK_EQ(speed->offset, static_cast<u32>(offsetof(PlayerStats, speed)));

    property_from_string(&stats, *speed, "42.5");
    NF_CHECK_NEAR(stats.speed, 42.5f, 1e-5f);

    const PropertyInfo* name = info->find_property("name");
    NF_CHECK(name != nullptr);
    NF_CHECK_EQ(name->offset, static_cast<u32>(offsetof(PlayerStats, name)));
    property_from_string(&stats, *name, "villain");
    NF_CHECK_EQ(stats.name, std::string("villain"));

    // A member that follows a std::string is the one a wrong offset would hit,
    // so check the tail of the struct too.
    const PropertyInfo* target = info->find_property("target");
    NF_CHECK(target != nullptr);
    NF_CHECK_EQ(target->offset, static_cast<u32>(offsetof(PlayerStats, target)));
    property_from_string(&stats, *target, "17");
    NF_CHECK_EQ(stats.target, 17u);
}

NF_TEST(reflection_sizes_match_the_member) {
    const ClassInfo* info = PlayerStats::nf_class_meta();
    NF_CHECK(info != nullptr);

    NF_CHECK_EQ(info->find_property("speed")->size, static_cast<u32>(sizeof(f32)));
    NF_CHECK_EQ(info->find_property("spawn")->size, static_cast<u32>(sizeof(Vec3)));
    NF_CHECK_EQ(info->find_property("facing")->size, static_cast<u32>(sizeof(Quat)));
    NF_CHECK_EQ(info->find_property("alive")->size, static_cast<u32>(sizeof(bool)));
    NF_CHECK_EQ(info->find_property("name")->size, static_cast<u32>(sizeof(std::string)));
}

NF_TEST(reflection_inherited_properties_are_visible) {
    const ClassInfo* info = PlayerStats::nf_class_meta();

    // Eight own properties plus two inherited.
    NF_CHECK_EQ(info->property_count, 8u);
    NF_CHECK_EQ(info->total_property_count(), 10u);

    // find_property walks the parent chain, so an inherited name resolves.
    const PropertyInfo* health = info->find_property("health");
    NF_CHECK(health != nullptr);
    NF_CHECK_EQ(std::string(health->category), std::string("Stats"));

    std::vector<const PropertyInfo*> all;
    info->collect_properties(all);
    NF_CHECK_EQ(all.size(), static_cast<usize>(10));

    // Most-derived first, so a shadowing property would win in the inspector.
    NF_CHECK_EQ(std::string(all.front()->name), std::string("speed"));
    NF_CHECK_EQ(std::string(all.back()->name), std::string("lives"));
}

NF_TEST(reflection_is_a_walks_the_chain) {
    const ClassInfo* player = PlayerStats::nf_class_meta();
    const ClassInfo* base   = BaseStats::nf_class_meta();
    NF_CHECK(player->is_a(player));
    NF_CHECK(player->is_a(base));
    NF_CHECK(!base->is_a(player));
}

NF_TEST(reflection_categories_group_properties) {
    const ClassInfo* info = PlayerStats::nf_class_meta();
    std::vector<const PropertyInfo*> all;
    info->collect_properties(all);

    u32 movement = 0;
    u32 identity = 0;
    u32 diagnostics = 0;
    for (const PropertyInfo* prop : all) {
        const std::string_view category = prop->category != nullptr ? prop->category : "";
        if (category == "Movement")         ++movement;
        else if (category == "Identity")    ++identity;
        else if (category == "Diagnostics") ++diagnostics;
    }

    NF_CHECK_EQ(movement, 4u);     // speed, spawn, facing, mode
    NF_CHECK_EQ(identity, 2u);     // name, alive
    NF_CHECK_EQ(diagnostics, 1u);  // error
}

NF_TEST(reflection_flags_gate_edit_and_serialize_independently) {
    const ClassInfo* info = PlayerStats::nf_class_meta();

    const PropertyInfo* speed = info->find_property("speed");
    NF_CHECK(speed->has_flag(Prop_EditAnywhere));
    NF_CHECK(!speed->has_flag(Prop_SerializeField));

    // The inherited one carries both, and it is reached through the parent.
    const PropertyInfo* lives = info->find_property("lives");
    NF_CHECK(lives->has_flag(Prop_EditAnywhere));
    NF_CHECK(lives->has_flag(Prop_SerializeField));

    NF_CHECK(!speed->has_flag(Prop_Replicated));
}

NF_TEST(reflection_enum_registers_variants) {
    const EnumInfo* drive = ReflectionRegistry::instance().find_enum("DriveMode");
    NF_CHECK(drive != nullptr);
    NF_CHECK_EQ(drive->variant_count, 3u);

    const EnumInfo::Variant* reverse = drive->find_variant("Reverse");
    NF_CHECK(reverse != nullptr);
    NF_CHECK_EQ(reverse->value, static_cast<i64>(2));

    const EnumInfo::Variant* by_value = drive->find_value(1);
    NF_CHECK(by_value != nullptr);
    NF_CHECK_EQ(std::string(by_value->name), std::string("Forward"));

    NF_CHECK(drive->find_variant("Nope") == nullptr);
    NF_CHECK(drive->find_value(99) == nullptr);

    const EnumInfo* error = ReflectionRegistry::instance().find_enum("ErrorCode");
    NF_CHECK(error != nullptr);
    NF_CHECK_EQ(error->find_variant("Warning")->value, static_cast<i64>(-3));
}

NF_TEST(reflection_property_round_trips_every_type) {
    PlayerStats stats;
    const ClassInfo* info = PlayerStats::nf_class_meta();

    struct Case {
        const char* name;
        const char* text;
    };
    const Case cases[] = {
        {"speed",  "12.25"},
        {"spawn",  "-1.5 2.5 3.5"},
        {"facing", "0 0 0.5 0.5"},
        {"name",   "round trip"},
        {"alive",  "false"},
        {"mode",   "Reverse"},
        {"error",  "Warning"},
        {"target", "42"},
    };

    for (const Case& c : cases) {
        const PropertyInfo* prop = info->find_property(c.name);
        NF_CHECK(prop != nullptr);
        NF_CHECK(property_from_string(&stats, *prop, c.text));

        const std::string written = property_to_string(&stats, *prop);
        NF_CHECK_EQ(written, std::string(c.text));
    }

    // Spot-check that the values actually landed, not just that the text agrees.
    NF_CHECK_NEAR(stats.speed, 12.25f, 1e-5f);
    NF_CHECK(!stats.alive);
    NF_CHECK(stats.mode == DriveMode::Reverse);
    NF_CHECK(stats.error == ErrorCode::Warning);
    NF_CHECK_EQ(stats.target, 42u);
    NF_CHECK_NEAR(stats.spawn.z, 3.5f, 1e-5f);
    NF_CHECK_NEAR(stats.facing.w, 0.5f, 1e-5f);
}

NF_TEST(reflection_float_text_survives_a_round_trip_exactly) {
    // The save format writes floats as text, so the text form has to carry every
    // bit. These values are chosen because they are *not* exactly representable
    // in binary — a format that prints too few digits silently drifts on reload.
    const f32 values[] = {
        0.1f,
        1.0f / 3.0f,
        3.14159265358979f,
        1e-8f,
        123456.789f,
        -0.70710678f,
    };

    PlayerStats stats;
    const PropertyInfo* speed = PlayerStats::nf_class_meta()->find_property("speed");
    NF_CHECK(speed != nullptr);

    for (f32 original : values) {
        stats.speed = original;
        const std::string text = property_to_string(&stats, *speed);

        stats.speed = 0.0f;  // clobber, so a parse that silently no-ops fails
        NF_CHECK(property_from_string(&stats, *speed, text));
        NF_CHECK(stats.speed == original);
    }
}

NF_TEST(reflection_enum_round_trips_through_a_narrow_underlying_type) {
    // DriveMode is : u8. A naive read as i32 would read three bytes of the next
    // member; this is the test that catches that.
    PlayerStats stats;
    stats.mode = DriveMode::Forward;
    const PropertyInfo* mode = PlayerStats::nf_class_meta()->find_property("mode");
    NF_CHECK(mode != nullptr);
    NF_CHECK_EQ(mode->size, static_cast<u32>(1));

    NF_CHECK_EQ(property_to_string(&stats, *mode), std::string("Forward"));
    NF_CHECK(property_from_string(&stats, *mode, "Idle"));
    NF_CHECK(stats.mode == DriveMode::Idle);

    // And the neighbouring members must be untouched by the byte-wide write.
    stats.alive = true;
    stats.error = ErrorCode::None;
    NF_CHECK(property_from_string(&stats, *mode, "Reverse"));
    NF_CHECK(stats.alive);
    NF_CHECK(stats.error == ErrorCode::None);
}

NF_TEST(reflection_entity_ref_none_is_explicit) {
    PlayerStats stats;
    const PropertyInfo* target = PlayerStats::nf_class_meta()->find_property("target");
    NF_CHECK(target != nullptr);

    stats.target = u32_max;
    NF_CHECK_EQ(property_to_string(&stats, *target), std::string("none"));

    NF_CHECK(property_from_string(&stats, *target, "none"));
    NF_CHECK_EQ(stats.target, u32_max);

    NF_CHECK(property_from_string(&stats, *target, "5"));
    NF_CHECK_EQ(stats.target, 5u);
}

NF_TEST(reflection_malformed_text_is_rejected_without_clobbering) {
    PlayerStats stats;
    stats.speed = 3.0f;
    stats.mode  = DriveMode::Forward;
    const ClassInfo* info = PlayerStats::nf_class_meta();

    const PropertyInfo* speed = info->find_property("speed");
    NF_CHECK(!property_from_string(&stats, *speed, "not a number"));
    NF_CHECK_NEAR(stats.speed, 3.0f, 1e-5f);

    const PropertyInfo* spawn = info->find_property("spawn");
    NF_CHECK(!property_from_string(&stats, *spawn, "1 2"));

    const PropertyInfo* alive = info->find_property("alive");
    NF_CHECK(!property_from_string(&stats, *alive, "maybe"));

    // An unregistered variant name is not an enum value, and is not a number
    // either, so it must be refused rather than silently becoming 0.
    const PropertyInfo* mode = info->find_property("mode");
    NF_CHECK(!property_from_string(&stats, *mode, "Sideways"));
    NF_CHECK(stats.mode == DriveMode::Forward);
}

NF_TEST(reflection_unknown_class_and_property_are_null_not_fatal) {
    NF_CHECK(ReflectionRegistry::instance().find_class("NoSuchClass") == nullptr);
    NF_CHECK(ReflectionRegistry::instance().find_enum("NoSuchEnum") == nullptr);
    NF_CHECK(PlayerStats::nf_class_meta()->find_property("nope") == nullptr);

    PropertyInfo empty{};
    PlayerStats stats;
    NF_CHECK_EQ(property_to_string(&stats, empty), std::string(""));
    NF_CHECK(!property_from_string(&stats, empty, "x"));
}

NF_TEST(reflection_registry_lists_registered_classes) {
    const auto& classes = ReflectionRegistry::instance().classes();
    bool found_player = false;
    bool found_base   = false;
    for (const ClassInfo* info : classes) {
        if (std::string_view(info->name) == "PlayerStats") found_player = true;
        if (std::string_view(info->name) == "BaseStats")   found_base = true;
    }
    NF_CHECK(found_player);
    NF_CHECK(found_base);
}
