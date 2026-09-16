// GameplayTests — dialogue trees: flow, gating, validation, saves.

#include <NF/Gameplay/Dialogue.hpp>
#include <NF/Gameplay/Tags.hpp>
#include <NF/Test/TestFramework.hpp>

#include <string>

using namespace nf;
using namespace nf::gameplay;

namespace {

DialogueTree make_quest_dialogue() {
    DialogueTree tree;
    DialogueNode hello;
    hello.id = "hello";
    hello.speaker = "Elder";
    hello.text = "Wolves trouble our flocks.";
    DialogueChoice ask;
    ask.id = "ask";
    ask.text = "Tell me more.";
    ask.next_node = "details";
    DialogueChoice accept;
    accept.id = "accept";
    accept.text = "I will hunt them.";
    accept.next_node = "farewell";
    accept.sets_tag = "quest.hunt.accepted";
    hello.choices.push_back(ask);
    hello.choices.push_back(accept);
    tree.add_node(hello);

    DialogueNode details;
    details.id = "details";
    details.speaker = "Elder";
    details.text = "Five of them, in the dark wood.";
    details.next = "hello"; // hub loop back
    tree.add_node(details);

    DialogueNode farewell;
    farewell.id = "farewell";
    farewell.speaker = "Elder";
    farewell.text = "Go with the wind.";
    tree.add_node(farewell); // terminal: no next, no choices
    return tree;
}

} // namespace

NF_TEST(dialogue_linear_run_with_hub_loop) {
    DialogueTree tree = make_quest_dialogue();
    std::string err;
    NF_CHECK(tree.validate("hello", err));
    NF_CHECK(err.empty());

    TagContainer tags;
    DialogueRunner run;
    NF_CHECK(run.start(tree, "hello"));
    NF_CHECK(!run.ended());
    NF_CHECK(run.current()->id == "hello");
    NF_CHECK(run.available_choices(tags).size() == 2);

    NF_CHECK(run.choose("ask", tags));
    NF_CHECK(run.current()->id == "details");
    NF_CHECK(run.available_choices(tags).empty());
    NF_CHECK(run.advance()); // linear next -> back to hello
    NF_CHECK(run.current()->id == "hello");

    NF_CHECK(run.choose("accept", tags));
    NF_CHECK(tags.has("quest.hunt.accepted")); // choice granted the tag
    NF_CHECK(run.current()->id == "farewell");
    NF_CHECK(!run.advance() || run.ended()); // terminal: advance ends
    NF_CHECK(run.ended());
    NF_CHECK(!run.choose("ask", tags)); // ended runs stay ended
}

NF_TEST(dialogue_choice_gating_by_tags) {
    DialogueTree tree;
    DialogueNode gate;
    gate.id = "gate";
    gate.speaker = "Guard";
    gate.text = "State your business.";
    DialogueChoice pass;
    pass.id = "pass";
    pass.text = "[Show the seal] Let me through.";
    pass.requires_tag = "item.seal";
    pass.next_node = "inside";
    DialogueChoice leave;
    leave.id = "leave";
    leave.text = "Never mind.";
    gate.choices.push_back(pass);
    gate.choices.push_back(leave);
    tree.add_node(gate);
    DialogueNode inside;
    inside.id = "inside";
    inside.speaker = "Guard";
    inside.text = "Welcome.";
    tree.add_node(inside);

    TagContainer tags;
    DialogueRunner run;
    NF_CHECK(run.start(tree, "gate"));
    NF_CHECK(run.available_choices(tags).size() == 1); // only "leave"
    NF_CHECK(!run.choose("pass", tags)); // gated: rejected
    tags.add("item.seal");
    NF_CHECK(run.available_choices(tags).size() == 2);
    NF_CHECK(run.choose("pass", tags));
    NF_CHECK(run.current()->id == "inside");
}

NF_TEST(dialogue_validation_names_problems) {
    DialogueTree tree;
    DialogueNode a;
    a.id = "a";
    a.next = "missing";
    tree.add_node(a);
    std::string err;
    NF_CHECK(!tree.validate("a", err));
    NF_CHECK(!err.empty());
    NF_CHECK(!tree.validate("nope", err));

    DialogueTree both;
    DialogueNode b;
    b.id = "b";
    b.next = "b";
    DialogueChoice c;
    c.id = "c";
    b.choices.push_back(c);
    both.add_node(b);
    NF_CHECK(!both.validate("b", err)); // next + choices together

    DialogueNode dup = b;
    NF_CHECK(!both.add_node(dup)); // duplicate id
    DialogueNode empty;
    NF_CHECK(!both.add_node(empty)); // empty id
}

NF_TEST(dialogue_serialize_roundtrip) {
    DialogueTree tree = make_quest_dialogue();
    DialogueTree restored;
    NF_CHECK(restored.parse(tree.serialize()));
    NF_CHECK(restored.node_count() == 3);
    std::string err;
    NF_CHECK(restored.validate("hello", err));
    const DialogueNode* hello = restored.find("hello");
    NF_CHECK(hello);
    NF_CHECK(hello->speaker == "Elder");
    NF_CHECK(hello->choices.size() == 2);
    NF_CHECK(hello->choices[1].sets_tag == "quest.hunt.accepted");

    DialogueTree tolerant;
    NF_CHECK(tolerant.parse("junk\nnode|bad\n" + tree.serialize()));
    NF_CHECK(tolerant.node_count() == 3);
}
