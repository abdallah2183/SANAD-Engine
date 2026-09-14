// Tests/EditorTests/test_reflected_inspector.cpp — Phase 10, W3
//
// The inspector's reflection layer decides *what* to draw, not how to draw it:
// which properties are visible, which widget each one gets, and how they group
// under headers. Those are the decisions worth testing, and they are testable
// precisely because NFEditorCore has no window.
//
// The ImGui shell is not covered here. What it does with this model is a direct
// walk with no policy in it — if a widget were wrong, the widget kind asserted
// below would be the thing that is wrong.

#include <NF/Test/TestFramework.hpp>

#include <NF/Editor/ReflectedInspector.hpp>

#include <NF/Core/Math.hpp>
#include <NF/Core/Reflection.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::editor;

namespace {

enum class CameraMode : u8 {
    Free  = 0,
    Orbit = 1,
    Track = 2,
};

struct InspectBase {
    f32 health = 100.0f;

    NF_CLASS(InspectBase)
    NF_PROPERTY(InspectBase, health, Float, Prop_EditAnywhere, "Stats")
    NF_CLASS_END(InspectBase)
};

struct InspectTarget {
    f32         speed      = 2.5f;
    i32         count      = 3;
    bool        enabled    = true;
    std::string name       = "target";
    Vec3        offset     = Vec3{1.0f, 2.0f, 3.0f};
    Quat        rotation   = Quat::identity();
    u32         target     = u32_max;
    CameraMode  mode       = CameraMode::Orbit;
    f32         hidden     = 99.0f;  // EditAnywhere is absent, so invisible
    f32         serialized = 7.0f;   // SerializeField only, so also invisible

    NF_CLASS(InspectTarget)
    NF_PROPERTY(InspectTarget, speed,      Float,     Prop_EditAnywhere, "Movement")
    NF_PROPERTY(InspectTarget, count,      Int,       Prop_EditAnywhere, "Movement")
    NF_PROPERTY(InspectTarget, enabled,    Bool,      Prop_EditAnywhere, "State")
    NF_PROPERTY(InspectTarget, name,       String,    Prop_EditAnywhere, "State")
    NF_PROPERTY(InspectTarget, offset,     Vec3,      Prop_EditAnywhere, "Placement")
    NF_PROPERTY(InspectTarget, rotation,   Quat,      Prop_EditAnywhere, "Placement")
    NF_PROPERTY(InspectTarget, target,     EntityRef, Prop_EditAnywhere, "Links")
    NF_PROPERTY_ENUM(InspectTarget, mode,  Enum,      Prop_EditAnywhere, "State", "CameraMode")
    NF_PROPERTY(InspectTarget, hidden,     Float,     Prop_None,         "Hidden")
    NF_PROPERTY(InspectTarget, serialized, Float,     Prop_SerializeField, "Hidden")
    NF_CLASS_END_DERIVED(InspectTarget, InspectBase)
};

/// A type with no reflection at all, standing in for the hand-written panels.
struct UnreflectedTarget {
    f32 value = 1.0f;
};

const ReflectedField* find_field(const ReflectedObjectView& view, const std::string& label) {
    for (const ReflectedField& field : view.fields()) {
        if (field.label == label) return &field;
    }
    return nullptr;
}

} // namespace

NF_ENUM_BEGIN(CameraMode)
    NF_ENUM_VALUE(CameraMode, Free)
    NF_ENUM_VALUE(CameraMode, Orbit)
    NF_ENUM_VALUE(CameraMode, Track)
NF_ENUM_END(CameraMode, "CameraMode")

NF_TEST(reflected_view_lists_only_edit_anywhere_properties) {
    InspectTarget target;
    const ReflectedObjectView view =
        ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    NF_CHECK(view.valid());
    // Eight own `EditAnywhere` properties plus the one inherited from InspectBase.
    NF_CHECK_EQ(view.fields().size(), static_cast<usize>(9));

    // `hidden` is Prop_None and `serialized` is SerializeField-only. Neither may
    // appear: a property being saved is not the same as a property being editable.
    NF_CHECK(find_field(view, "hidden") == nullptr);
    NF_CHECK(find_field(view, "serialized") == nullptr);
    NF_CHECK(find_field(view, "speed") != nullptr);
}

NF_TEST(reflected_view_maps_every_type_to_a_widget) {
    InspectTarget target;
    const ReflectedObjectView view =
        ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    struct Expectation {
        const char*         label;
        ReflectedWidgetKind widget;
    };
    const Expectation expectations[] = {
        {"speed",    ReflectedWidgetKind::FloatDrag},
        {"count",    ReflectedWidgetKind::IntDrag},
        {"enabled",  ReflectedWidgetKind::BoolCheckbox},
        {"name",     ReflectedWidgetKind::TextInput},
        {"offset",   ReflectedWidgetKind::Vec3Drag},
        {"rotation", ReflectedWidgetKind::QuatEulerDrag},
        {"target",   ReflectedWidgetKind::EntityPicker},
        {"mode",     ReflectedWidgetKind::EnumCombo},
        {"health",   ReflectedWidgetKind::FloatDrag},  // inherited, same mapping
    };

    for (const Expectation& e : expectations) {
        const ReflectedField* field = find_field(view, e.label);
        NF_CHECK(field != nullptr);
        if (field != nullptr) {
            NF_CHECK_EQ(static_cast<int>(field->widget), static_cast<int>(e.widget));
        }
    }

    // Types with no widget must say so rather than silently vanishing.
    NF_CHECK(widget_for(PropertyType::Array) == ReflectedWidgetKind::Unsupported);
    NF_CHECK(widget_for(PropertyType::Unknown) == ReflectedWidgetKind::Unsupported);
}

NF_TEST(reflected_view_groups_by_category_in_first_appearance_order) {
    InspectTarget target;
    const ReflectedObjectView view =
        ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    const std::vector<ReflectedGroup> groups = view.groups();

    // Declaration order is Movement, State, Placement, Links, then the inherited
    // Stats — not alphabetical, because the author's ordering is the useful one.
    NF_CHECK_EQ(groups.size(), static_cast<usize>(5));
    if (groups.size() == 5) {
        NF_CHECK_EQ(groups[0].category, std::string("Movement"));
        NF_CHECK_EQ(groups[1].category, std::string("State"));
        NF_CHECK_EQ(groups[2].category, std::string("Placement"));
        NF_CHECK_EQ(groups[3].category, std::string("Links"));
        NF_CHECK_EQ(groups[4].category, std::string("Stats"));

        NF_CHECK_EQ(groups[0].field_indices.size(), static_cast<usize>(2));  // speed, count
        NF_CHECK_EQ(groups[1].field_indices.size(), static_cast<usize>(3));  // enabled, name, mode
        NF_CHECK_EQ(groups[2].field_indices.size(), static_cast<usize>(2));  // offset, rotation
        NF_CHECK_EQ(groups[3].field_indices.size(), static_cast<usize>(1));  // target
        NF_CHECK_EQ(groups[4].field_indices.size(), static_cast<usize>(1));  // health
    }

    // Every field must appear in exactly one group, or the panel would drop it.
    usize grouped = 0;
    for (const ReflectedGroup& group : groups) grouped += group.field_indices.size();
    NF_CHECK_EQ(grouped, view.fields().size());
}

NF_TEST(reflected_view_carries_all_enum_variants_in_declaration_order) {
    InspectTarget target;
    const ReflectedObjectView view =
        ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    const ReflectedField* mode = find_field(view, "mode");
    NF_CHECK(mode != nullptr);
    if (mode != nullptr) {
        NF_CHECK_EQ(mode->enum_variants.size(), static_cast<usize>(3));
        if (mode->enum_variants.size() == 3) {
            NF_CHECK_EQ(mode->enum_variants[0], std::string("Free"));
            NF_CHECK_EQ(mode->enum_variants[1], std::string("Orbit"));
            NF_CHECK_EQ(mode->enum_variants[2], std::string("Track"));
        }
        NF_CHECK_EQ(mode->value, std::string("Orbit"));
    }

    // A non-enum field must not carry variants — a combo with stale entries is
    // worse than no combo.
    const ReflectedField* speed = find_field(view, "speed");
    NF_CHECK(speed != nullptr);
    if (speed != nullptr) NF_CHECK(speed->enum_variants.empty());
}

NF_TEST(reflected_view_reads_and_writes_through_to_the_live_object) {
    InspectTarget target;
    ReflectedObjectView view = ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    usize speed_index = view.fields().size();
    for (usize i = 0; i < view.fields().size(); ++i) {
        if (view.fields()[i].label == "speed") speed_index = i;
    }
    NF_CHECK(speed_index < view.fields().size());
    NF_CHECK_EQ(view.value_of(speed_index), std::string("2.5"));

    NF_CHECK(view.set_value(speed_index, "18.25"));
    // The write must land on the object, not just in the view's cache.
    NF_CHECK_NEAR(target.speed, 18.25f, 1e-5f);
    NF_CHECK_EQ(view.value_of(speed_index), std::string("18.25"));
}

NF_TEST(reflected_view_rejects_malformed_input_without_touching_the_object) {
    InspectTarget target;
    ReflectedObjectView view = ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    usize speed_index = view.fields().size();
    usize mode_index = view.fields().size();
    for (usize i = 0; i < view.fields().size(); ++i) {
        if (view.fields()[i].label == "speed") speed_index = i;
        if (view.fields()[i].label == "mode")  mode_index = i;
    }

    NF_CHECK(!view.set_value(speed_index, "not a number"));
    NF_CHECK_NEAR(target.speed, 2.5f, 1e-5f);

    NF_CHECK(!view.set_value(mode_index, "Sideways"));
    NF_CHECK(target.mode == CameraMode::Orbit);

    // Out of range is refused rather than clamping onto a neighbour.
    NF_CHECK(!view.set_value(view.fields().size(), "1.0"));
    NF_CHECK_EQ(view.value_of(view.fields().size()), std::string(""));
}

NF_TEST(reflected_view_refresh_picks_up_changes_made_elsewhere) {
    InspectTarget target;
    ReflectedObjectView view = ReflectedObjectView::build(&target, InspectTarget::nf_class_meta());

    usize name_index = view.fields().size();
    for (usize i = 0; i < view.fields().size(); ++i) {
        if (view.fields()[i].label == "name") name_index = i;
    }
    NF_CHECK_EQ(view.fields()[name_index].value, std::string("target"));

    // Something else edits the object — an undo, a gameplay module, a hot reload.
    target.name = "renamed elsewhere";
    NF_CHECK_EQ(view.fields()[name_index].value, std::string("target"));  // cache is stale

    view.refresh();
    NF_CHECK_EQ(view.fields()[name_index].value, std::string("renamed elsewhere"));
}

NF_TEST(reflected_view_is_inert_without_metadata) {
    UnreflectedTarget plain;

    // This is what lets the panel fall back to the hand-written editors: a type
    // that never opted into reflection produces an invalid view, not a crash and
    // not an empty-looking panel with no explanation.
    ReflectedObjectView no_meta = ReflectedObjectView::build(&plain, nullptr);
    NF_CHECK(!no_meta.valid());
    NF_CHECK(no_meta.empty());
    NF_CHECK(no_meta.groups().empty());
    NF_CHECK(!no_meta.set_value(0, "1"));

    const ReflectedObjectView no_instance =
        ReflectedObjectView::build(nullptr, InspectTarget::nf_class_meta());
    NF_CHECK(!no_instance.valid());
    NF_CHECK(no_instance.empty());
}

NF_TEST(gameplay_module_candidates_are_sorted_and_stable) {
    const std::vector<std::string> names = gameplay_module_candidates();

    // The dropdown is built from this list, so it must not reshuffle between
    // runs. Registration order is unspecified, which is why the registry sorts.
    for (usize i = 1; i < names.size(); ++i) {
        NF_CHECK(names[i - 1] <= names[i]);
    }

    // Calling twice must give the same answer.
    NF_CHECK(gameplay_module_candidates() == names);
}
