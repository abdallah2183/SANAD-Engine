// GameplayTests — tags, quests, inventory: rules data, no GPU, no Runtime.

#include <NF/Test/TestFramework.hpp>

#include <NF/Gameplay/Inventory.hpp>
#include <NF/Gameplay/Quest.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>

using namespace nf;
using namespace nf::gameplay;

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

NF_TEST(tags_prefix_matching) {
    NF_CHECK(tag_matches_prefix("enemy", "enemy"));
    NF_CHECK(tag_matches_prefix("enemy.boss", "enemy"));
    NF_CHECK(!tag_matches_prefix("enemy", "enemy.boss")); // ancestor ≠ descendant
    NF_CHECK(!tag_matches_prefix("enemyX", "enemy")); // segment boundary matters
    NF_CHECK(!tag_matches_prefix("enem", "enemy"));
    NF_CHECK(!tag_matches_prefix("enemy", ""));
}

NF_TEST(tags_container_hierarchy) {
    TagContainer tags;
    tags.add("enemy.boss.fire");
    tags.add("lootable");
    NF_CHECK(tags.has("enemy.boss.fire"));
    NF_CHECK(tags.has("enemy.boss")); // parent implied
    NF_CHECK(tags.has("enemy")); // grandparent implied
    NF_CHECK(tags.has("lootable"));
    NF_CHECK(!tags.has("enemy.boss.ice"));
    NF_CHECK(!tags.has("enemy.boss.fire.extra")); // held ancestor ≠ query descendant
    NF_CHECK(!tags.has("friend"));
    NF_CHECK(tags.has_exact("enemy.boss.fire"));
    NF_CHECK(!tags.has_exact("enemy"));

    tags.remove("enemy.boss.fire");
    NF_CHECK(!tags.has("enemy"));
    NF_CHECK(tags.has("lootable"));
    NF_CHECK(tags.size() == 1);
    NF_CHECK(!tags.empty());
    tags.clear();
    NF_CHECK(tags.empty());
}

NF_TEST(tags_query_language) {
    TagContainer tags;
    tags.add("enemy.boss");
    tags.add("vulnerable.fire");

    auto q1 = TagQuery::parse("enemy & vulnerable.fire");
    NF_CHECK(q1.ok());
    NF_CHECK(q1.matches(tags));

    auto q2 = TagQuery::parse("enemy.boss & !vulnerable.ice");
    NF_CHECK(q2.ok());
    NF_CHECK(q2.matches(tags));

    auto q3 = TagQuery::parse("player | enemy.boss");
    NF_CHECK(q3.ok());
    NF_CHECK(q3.matches(tags));

    auto q4 = TagQuery::parse("player & enemy");
    NF_CHECK(q4.ok());
    NF_CHECK(!q4.matches(tags));

    auto q5 = TagQuery::parse("(player | enemy) & vulnerable.fire");
    NF_CHECK(q5.ok());
    NF_CHECK(q5.matches(tags));

    // Invalid queries fail closed (match nothing, never everything).
    for (const char* bad : {"", "   ", "enemy &", "& enemy", "!", "(enemy", "enemy)", "a..b",
                            ".enemy", "enemy.", "enemy | | boss"}) {
        auto q = TagQuery::parse(bad);
        NF_CHECK(!q.ok());
        NF_CHECK(!q.error().empty());
        NF_CHECK(!q.matches(tags));
    }
    TagContainer empty;
    NF_CHECK(!q1.matches(empty));
}

// ---------------------------------------------------------------------------
// Quests
// ---------------------------------------------------------------------------

namespace {

Quest make_hunt_quest() {
    Quest q;
    q.id = "hunt";
    q.title = "Wolf Hunt";
    QuestStage s1;
    s1.id = "track";
    s1.label = "Track the pack";
    QuestObjective o1;
    o1.id = "tracks";
    o1.label = "Find tracks";
    o1.target = 3;
    s1.objectives.push_back(o1);
    QuestStage s2;
    s2.id = "slay";
    s2.label = "Slay the wolves";
    QuestObjective o2;
    o2.id = "wolves";
    o2.label = "Wolves slain";
    o2.target = 5;
    s2.objectives.push_back(o2);
    q.stages.push_back(s1);
    q.stages.push_back(s2);
    return q;
}

} // namespace

NF_TEST(quest_full_lifecycle) {
    QuestLog log;
    NF_CHECK(log.define(make_hunt_quest()));
    NF_CHECK(!log.define(make_hunt_quest())); // duplicate id rejected
    Quest empty_id;
    NF_CHECK(!log.define(empty_id));

    NF_CHECK(log.active_count() == 0);
    NF_CHECK(log.start("hunt"));
    NF_CHECK(!log.start("hunt")); // already active
    NF_CHECK(!log.start("missing"));
    NF_CHECK(log.active_count() == 1);

    // Wrong objective / wrong quest / zero amount: all rejected.
    NF_CHECK(!log.advance("hunt", "wolves")); // stage 1 wants tracks
    NF_CHECK(!log.advance("missing", "tracks"));
    NF_CHECK(!log.advance("hunt", "tracks", 0));

    NF_CHECK(log.advance("hunt", "tracks", 2));
    const Quest* q = log.find("hunt");
    NF_CHECK(q && q->stage_index == 0);
    NF_CHECK(log.advance("hunt", "tracks", 9)); // saturates at target 3
    NF_CHECK(q->stage_index == 1); // advanced to stage 2
    NF_CHECK(log.active_count() == 1);

    NF_CHECK(log.advance("hunt", "wolves", 5));
    NF_CHECK(q->complete());
    NF_CHECK(log.completed_count() == 1);
    NF_CHECK(log.active_count() == 0);
    NF_CHECK(!log.advance("hunt", "wolves")); // completed quests ignore progress
}

NF_TEST(quest_fail_and_milestone_stages) {
    QuestLog log;
    Quest q;
    q.id = "escort";
    q.title = "Escort";
    QuestStage milestone; // no objectives: arriving completes it
    milestone.id = "arrive";
    QuestStage fight;
    fight.id = "fight";
    QuestObjective o;
    o.id = "bandits";
    o.target = 2;
    fight.objectives.push_back(o);
    q.stages.push_back(milestone);
    q.stages.push_back(fight);
    NF_CHECK(log.define(q));
    NF_CHECK(log.start("escort"));
    // Milestone skipped at start: stage_index 1 already.
    NF_CHECK(log.find("escort")->stage_index == 1);
    NF_CHECK(log.fail("escort"));
    NF_CHECK(log.find("escort")->state == QuestState::Failed);
    NF_CHECK(!log.fail("escort")); // only active quests fail
    NF_CHECK(!log.advance("escort", "bandits"));
}

NF_TEST(quest_serialize_roundtrip) {
    QuestLog log;
    NF_CHECK(log.define(make_hunt_quest()));
    NF_CHECK(log.start("hunt"));
    NF_CHECK(log.advance("hunt", "tracks", 3));
    NF_CHECK(log.advance("hunt", "wolves", 2));

    QuestLog restored;
    NF_CHECK(restored.parse(log.serialize()));
    NF_CHECK(restored.quest_count() == 1);
    const Quest* q = restored.find("hunt");
    NF_CHECK(q);
    NF_CHECK(q->title == "Wolf Hunt");
    NF_CHECK(q->state == QuestState::Active);
    NF_CHECK(q->stage_index == 1);
    NF_CHECK(q->stages[0].objectives[0].progress == 3);
    NF_CHECK(q->stages[1].objectives[0].progress == 2);

    // Garbage lines are skipped, valid ones survive.
    QuestLog tolerant;
    NF_CHECK(tolerant.parse("garbage line\nquest|bad\n" + log.serialize()));
    NF_CHECK(tolerant.quest_count() == 1);
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

NF_TEST(inventory_stacking_and_capacity) {
    Inventory inv(2); // 2 stacks max
    // 50 arrows at stack 20: two full stacks (40), 10 rejected (capacity).
    NF_CHECK(inv.add("arrow", 50, 20) == 40);
    NF_CHECK(inv.count_of("arrow") == 40);
    NF_CHECK(inv.stack_count() == 2);
    NF_CHECK(inv.add("arrow", 5, 20) == 0); // full
    NF_CHECK(inv.remove("arrow", 15) == 15);
    NF_CHECK(inv.count_of("arrow") == 25);
    NF_CHECK(inv.add("arrow", 5, 20) == 5); // room again
    NF_CHECK(inv.has("arrow", 30));
    NF_CHECK(!inv.has("arrow", 31));
    NF_CHECK(inv.remove("arrow", 999) == 30); // drains everything
    NF_CHECK(inv.empty());
    NF_CHECK(inv.add("", 5) == 0);
    NF_CHECK(inv.add("x", 0) == 0);
}

NF_TEST(inventory_sorted_and_serialized) {
    Inventory inv;
    NF_CHECK(inv.add("sword", 1) == 1);
    NF_CHECK(inv.add("arrow", 10) == 10);
    NF_CHECK(inv.add("potion", 3, 5) == 3);
    const auto& stacks = inv.stacks();
    NF_CHECK(stacks.size() == 3);
    NF_CHECK(stacks[0].id == "arrow"); // sorted by id
    NF_CHECK(stacks[1].id == "potion");
    NF_CHECK(stacks[2].id == "sword");

    Inventory restored;
    NF_CHECK(restored.parse(inv.serialize()));
    NF_CHECK(restored.count_of("arrow") == 10);
    NF_CHECK(restored.count_of("potion") == 3);
    NF_CHECK(restored.count_of("sword") == 1);
    NF_CHECK(restored.stacks()[0].id == "arrow");

    Inventory tolerant;
    NF_CHECK(tolerant.parse("junk\n|bad||\n" + inv.serialize()));
    NF_CHECK(tolerant.count_of("sword") == 1);
}
