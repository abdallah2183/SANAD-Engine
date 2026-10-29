#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/SaveGame.hpp>

#include <filesystem>
#include <fstream>

using namespace nf::gameplay;

NF_TEST(attributes_basic_modifiers) {
    AttributeSet set;
    NF_CHECK(set.register_attribute("Health", 100.0f, 0.0f, 200.0f));
    NF_CHECK(set.has_attribute("Health"));
    NF_CHECK_NEAR(set.get_current_value("Health"), 100.0f, 1e-5f);

    float last_old = 0.0f, last_new = 0.0f;
    set.on_change("Health", [&](const std::string&, float old_val, float new_val) {
        last_old = old_val;
        last_new = new_val;
    });

    // Add +20 flat buff (source 1)
    set.add_modifier("Health", AttributeModifier{ModifierOp::Add, 20.0f, 1});
    NF_CHECK_NEAR(set.get_current_value("Health"), 120.0f, 1e-5f);
    NF_CHECK_NEAR(last_old, 100.0f, 1e-5f);
    NF_CHECK_NEAR(last_new, 120.0f, 1e-5f);

    // Add x1.5 multiplier (source 2) -> (100 + 20) * 1.5 = 180
    set.add_modifier("Health", AttributeModifier{ModifierOp::Multiply, 1.5f, 2});
    NF_CHECK_NEAR(set.get_current_value("Health"), 180.0f, 1e-5f);

    // Remove source 1 -> (100) * 1.5 = 150
    NF_CHECK(set.remove_modifiers_by_source("Health", 1));
    NF_CHECK_NEAR(set.get_current_value("Health"), 150.0f, 1e-5f);

    // Remove source 2 -> 100
    NF_CHECK(set.remove_modifiers_by_source("Health", 2));
    NF_CHECK_NEAR(set.get_current_value("Health"), 100.0f, 1e-5f);
}

NF_TEST(attributes_override_and_clamp) {
    AttributeSet set;
    set.register_attribute("Speed", 10.0f, 0.0f, 30.0f);

    // Exceed max clamp
    set.add_modifier("Speed", AttributeModifier{ModifierOp::Add, 50.0f, 10});
    NF_CHECK_NEAR(set.get_current_value("Speed"), 30.0f, 1e-5f);

    // Override (e.g. frozen / immobilized)
    set.add_modifier("Speed", AttributeModifier{ModifierOp::Override, 0.0f, 20});
    NF_CHECK_NEAR(set.get_current_value("Speed"), 0.0f, 1e-5f);

    // Remove override
    set.remove_modifiers_by_source("Speed", 20);
    NF_CHECK_NEAR(set.get_current_value("Speed"), 30.0f, 1e-5f);
}

NF_TEST(effect_manager_duration_and_periodic) {
    AttributeSet set;
    set.register_attribute("Attack", 20.0f, 0.0f, 100.0f);
    set.register_attribute("Health", 50.0f, 0.0f, 100.0f);

    EffectManager mgr(set);

    // 1. Duration buff: +10 Attack for 2.0s
    GameplayEffect buff;
    buff.name = "Berserk";
    buff.duration_type = EffectDurationType::Duration;
    buff.duration = 2.0f;
    buff.source_id = 101;
    buff.modifiers.push_back({"Attack", AttributeModifier{ModifierOp::Add, 10.0f, 101}});

    mgr.apply_effect(buff);
    NF_CHECK_EQ(mgr.active_effect_count(), 1u);
    NF_CHECK_NEAR(set.get_current_value("Attack"), 30.0f, 1e-5f);

    mgr.update(1.0f);
    NF_CHECK_EQ(mgr.active_effect_count(), 1u);
    NF_CHECK_NEAR(set.get_current_value("Attack"), 30.0f, 1e-5f);

    // Expire buff
    mgr.update(1.1f);
    NF_CHECK_EQ(mgr.active_effect_count(), 0u);
    NF_CHECK_NEAR(set.get_current_value("Attack"), 20.0f, 1e-5f);

    // 2. Periodic Regeneration: +10 Health every 1.0s for 3.0s
    GameplayEffect regen;
    regen.name = "Regeneration";
    regen.duration_type = EffectDurationType::Periodic;
    regen.duration = 3.0f;
    regen.period = 1.0f;
    regen.source_id = 102;
    regen.instant_deltas.push_back({"Health", 10.0f});

    mgr.apply_effect(regen);
    NF_CHECK_NEAR(set.get_current_value("Health"), 60.0f, 1e-5f); // First tick instant

    mgr.update(1.0f);
    NF_CHECK_NEAR(set.get_current_value("Health"), 70.0f, 1e-5f); // Second tick

    mgr.update(1.0f);
    NF_CHECK_NEAR(set.get_current_value("Health"), 80.0f, 1e-5f); // Third tick

    mgr.update(1.1f); // Finished
    NF_CHECK_EQ(mgr.active_effect_count(), 0u);
    NF_CHECK_NEAR(set.get_current_value("Health"), 80.0f, 1e-5f);
}

NF_TEST(save_game_archive_roundtrip) {
    SaveGameArchive archive;
    archive.write_bool(true);
    archive.write_i32(-42);
    archive.write_u32(123456u);
    archive.write_i64(-9876543210LL);
    archive.write_u64(9876543210ULL);
    archive.write_f32(3.14159f);
    archive.write_f64(2.718281828459);
    archive.write_string("SANAD Engine Save Data");

    bool b = false;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    std::string str;

    NF_CHECK(archive.read_bool(b));
    NF_CHECK(archive.read_i32(i32));
    NF_CHECK(archive.read_u32(u32));
    NF_CHECK(archive.read_i64(i64));
    NF_CHECK(archive.read_u64(u64));
    NF_CHECK(archive.read_f32(f32));
    NF_CHECK(archive.read_f64(f64));
    NF_CHECK(archive.read_string(str));

    NF_CHECK(b);
    NF_CHECK_EQ(i32, -42);
    NF_CHECK_EQ(u32, 123456u);
    NF_CHECK_EQ(i64, -9876543210LL);
    NF_CHECK_EQ(u64, 9876543210ULL);
    NF_CHECK_NEAR(f32, 3.14159f, 1e-5f);
    NF_CHECK_NEAR(static_cast<float>(f64), 2.71828f, 1e-5f);
    NF_CHECK(str == "SANAD Engine Save Data");
}

NF_TEST(save_game_system_persistence_and_checksum) {
    const std::filesystem::path test_path = std::filesystem::temp_directory_path() / "sanad_test_save.sav";

    SaveGameArchive archive;
    archive.write_string("Level_01");
    archive.write_i32(999);
    archive.write_f32(125.5f);

    std::string err;
    NF_CHECK(SaveGameSystem::save_to_file(test_path, archive, err));
    NF_CHECK(SaveGameSystem::exists(test_path));

    SaveGameArchive loaded;
    NF_CHECK(SaveGameSystem::load_from_file(test_path, loaded, err));

    std::string level;
    int32_t score = 0;
    float hp = 0.0f;
    NF_CHECK(loaded.read_string(level));
    NF_CHECK(loaded.read_i32(score));
    NF_CHECK(loaded.read_f32(hp));

    NF_CHECK(level == "Level_01");
    NF_CHECK_EQ(score, 999);
    NF_CHECK_NEAR(hp, 125.5f, 1e-5f);

    // Corrupt the file payload
    {
        std::fstream file(test_path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(sizeof(uint32_t) * 4 + 2); // Inside payload
        char corrupt = 0x7F;
        file.write(&corrupt, 1);
    }

    SaveGameArchive corrupted;
    bool load_res = SaveGameSystem::load_from_file(test_path, corrupted, err);
    NF_CHECK(!load_res); // Checksum must reject corrupted payload
    NF_CHECK(!err.empty());

    SaveGameSystem::remove(test_path);
    NF_CHECK(!SaveGameSystem::exists(test_path));
}
