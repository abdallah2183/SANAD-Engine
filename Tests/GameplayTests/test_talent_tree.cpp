// Tests/GameplayTests/test_talent_tree.cpp — Skill & Talent Tree tests (Phase 122)

#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/TalentTree.hpp>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(talent_tree_initial_state) {
    TalentTree tree("warrior_spec", "Warrior Spec");
    NF_CHECK_EQ(tree.id(), "warrior_spec");
    NF_CHECK_EQ(tree.name(), "Warrior Spec");
    NF_CHECK_EQ(tree.available_points(), 0u);
    NF_CHECK_EQ(tree.total_spent_points(), 0u);
    NF_CHECK(tree.nodes().empty());
}

NF_TEST(talent_tree_add_node) {
    TalentTree tree("mage_spec", "Mage Spec");

    TalentNode node;
    node.id = "fireball_mastery";
    node.name = "Fireball Mastery";
    node.max_rank = 3;
    node.cost_per_rank = 1;
    node.stat_modifier_per_rank = 10.0f; // +10% fire damage per rank

    bool added = tree.add_node(node);
    NF_CHECK(added);
    NF_CHECK_EQ(tree.nodes().size(), 1u);

    const TalentNode* retrieved = tree.get_node("fireball_mastery");
    NF_CHECK(retrieved != nullptr);
    NF_CHECK_EQ(retrieved->max_rank, 3u);
}

NF_TEST(talent_tree_invest_point_success) {
    TalentTree tree("rogue_spec", "Rogue Spec");

    TalentNode node;
    node.id = "stealth_efficiency";
    node.max_rank = 2;
    tree.add_node(node);

    tree.add_points(3);
    NF_CHECK_EQ(tree.available_points(), 3u);

    bool ok = tree.invest_point("stealth_efficiency");
    NF_CHECK(ok);
    NF_CHECK_EQ(tree.available_points(), 2u);
    NF_CHECK_EQ(tree.total_spent_points(), 1u);

    const TalentNode* n = tree.get_node("stealth_efficiency");
    NF_CHECK_EQ(n->current_rank, 1u);
    NF_CHECK(n->is_unlocked());
    NF_CHECK(!n->is_maxed());
}

NF_TEST(talent_tree_insufficient_points_rejected) {
    TalentTree tree("test", "Test");
    TalentNode node;
    node.id = "slash";
    node.max_rank = 1;
    node.cost_per_rank = 2;
    tree.add_node(node);

    tree.add_points(1); // 1 point available, but node costs 2
    bool ok = tree.invest_point("slash");
    NF_CHECK(!ok);
    NF_CHECK_EQ(tree.available_points(), 1u);
    NF_CHECK_EQ(tree.total_spent_points(), 0u);
}

NF_TEST(talent_tree_max_rank_cap) {
    TalentTree tree("test", "Test");
    TalentNode node;
    node.id = "armor_up";
    node.max_rank = 2;
    tree.add_node(node);

    tree.add_points(10);
    NF_CHECK(tree.invest_point("armor_up")); // rank 1
    NF_CHECK(tree.invest_point("armor_up")); // rank 2 (max)
    NF_CHECK(!tree.invest_point("armor_up")); // rank 3 rejected!

    const TalentNode* n = tree.get_node("armor_up");
    NF_CHECK_EQ(n->current_rank, 2u);
    NF_CHECK(n->is_maxed());
}

NF_TEST(talent_tree_prerequisite_gating) {
    TalentTree tree("test", "Test");

    TalentNode root;
    root.id = "iron_skin";
    root.max_rank = 2;
    tree.add_node(root);

    TalentNode child;
    child.id = "unbreakable";
    child.max_rank = 1;
    child.prerequisites.push_back("iron_skin");
    tree.add_node(child);

    tree.add_points(5);

    // Child cannot be invested when root is rank 0
    NF_CHECK(!tree.invest_point("unbreakable"));

    // Root rank 1 (partial) still cannot unlock child
    NF_CHECK(tree.invest_point("iron_skin"));
    NF_CHECK(!tree.invest_point("unbreakable"));

    // Root rank 2 (maxed) now unlocks child
    NF_CHECK(tree.invest_point("iron_skin"));
    NF_CHECK(tree.invest_point("unbreakable"));

    const TalentNode* child_node = tree.get_node("unbreakable");
    NF_CHECK_EQ(child_node->current_rank, 1u);
}

NF_TEST(talent_tree_tier_point_requirement) {
    TalentTree tree("test", "Test");

    TalentNode t0;
    t0.id = "strike";
    t0.tier = 0;
    t0.max_rank = 3;
    t0.points_required_in_tree = 0;
    tree.add_node(t0);

    TalentNode t1;
    t1.id = "whirlwind";
    t1.tier = 1;
    t1.max_rank = 1;
    t1.points_required_in_tree = 3; // Requires 3 points spent
    tree.add_node(t1);

    tree.add_points(10);

    // Whirlwind rejected when spent points < 3
    NF_CHECK(!tree.invest_point("whirlwind"));

    // Invest 3 points into strike
    tree.invest_point("strike");
    tree.invest_point("strike");
    NF_CHECK(!tree.invest_point("whirlwind")); // spent = 2 < 3

    tree.invest_point("strike"); // spent = 3 >= 3
    NF_CHECK(tree.invest_point("whirlwind")); // now accepted!
}

NF_TEST(talent_tree_refund_point) {
    TalentTree tree("test", "Test");
    TalentNode node;
    node.id = "agility";
    node.max_rank = 3;
    tree.add_node(node);

    tree.add_points(3);
    tree.invest_point("agility");
    tree.invest_point("agility");
    NF_CHECK_EQ(tree.available_points(), 1u);
    NF_CHECK_EQ(tree.total_spent_points(), 2u);

    bool refunded = tree.refund_point("agility");
    NF_CHECK(refunded);
    NF_CHECK_EQ(tree.available_points(), 2u);
    NF_CHECK_EQ(tree.total_spent_points(), 1u);
    NF_CHECK_EQ(tree.get_node("agility")->current_rank, 1u);
}

NF_TEST(talent_tree_refund_blocked_by_dependency) {
    TalentTree tree("test", "Test");

    TalentNode root;
    root.id = "root_node";
    root.max_rank = 1;
    tree.add_node(root);

    TalentNode leaf;
    leaf.id = "leaf_node";
    leaf.max_rank = 1;
    leaf.prerequisites.push_back("root_node");
    tree.add_node(leaf);

    tree.add_points(5);
    tree.invest_point("root_node");
    tree.invest_point("leaf_node");

    // Cannot refund root_node because leaf_node depends on it
    NF_CHECK(!tree.refund_point("root_node"));

    // Refunding leaf_node first allows root_node to be refunded
    NF_CHECK(tree.refund_point("leaf_node"));
    NF_CHECK(tree.refund_point("root_node"));
}

NF_TEST(talent_tree_active_tags_and_serialization) {
    TalentTree tree("paladin", "Paladin Holy");

    TalentNode n1;
    n1.id = "blessing_of_light";
    n1.granted_tag = "Talent.Holy.Light";
    n1.granted_ability = "Ability_HolyLight";
    n1.max_rank = 1;
    tree.add_node(n1);

    TalentNode n2;
    n2.id = "aura_of_protection";
    n2.granted_tag = "Talent.Holy.Armor";
    n2.max_rank = 2;
    tree.add_node(n2);

    tree.add_points(3);
    tree.invest_point("blessing_of_light");
    tree.invest_point("aura_of_protection");

    auto tags = tree.get_active_tags();
    NF_CHECK_EQ(tags.size(), 2u);

    auto abilities = tree.get_active_abilities();
    NF_CHECK_EQ(abilities.size(), 1u);
    NF_CHECK_EQ(abilities[0], "Ability_HolyLight");

    std::string text = tree.serialize();
    NF_CHECK(!text.empty());

    // Restore into fresh tree with same definitions
    TalentTree restored("paladin", "Paladin Holy");
    restored.add_node(n1);
    restored.add_node(n2);

    bool ok = restored.parse(text);
    NF_CHECK(ok);
    NF_CHECK_EQ(restored.available_points(), 1u);
    NF_CHECK_EQ(restored.total_spent_points(), 2u);
    NF_CHECK_EQ(restored.get_node("blessing_of_light")->current_rank, 1u);
    NF_CHECK_EQ(restored.get_node("aura_of_protection")->current_rank, 1u);
}
