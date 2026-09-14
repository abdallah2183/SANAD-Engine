#pragma once

// NF/Editor/ReflectedInspector.hpp — reflection-driven inspector model (Phase 10, W3)
//
// The design doc's §249 S1 promises properties that are "visible and editable in
// the inspector without hand-written panel code". This is the half of that
// promise that can be tested without a window: it turns a reflected object into
// a flat list of labelled fields with a widget kind each, and a set of groups
// for the category headers.
//
// The split matters. `NFEditorCore` is deliberately window-free and RHI-free, so
// the *decisions* — which widget, which category, which properties are visible —
// live here and are covered by EditorTests. The ImGui shell in
// `Editor/src/ui/Panels.cpp` only walks this model and calls the matching widget;
// it contains no policy worth testing.

#include <NF/Core/Reflection.hpp>
#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace nf::editor {

/// Which control the panel should draw for a property. `Unsupported` means the
/// type is reflected but has no widget — the panel shows it read-only rather
/// than silently dropping it, so a missing widget is visible instead of being
/// mistaken for a missing property.
enum class ReflectedWidgetKind : u8 {
    Unsupported = 0,
    FloatDrag,
    IntDrag,
    BoolCheckbox,
    TextInput,
    Vec3Drag,
    QuatEulerDrag,  // edited in degrees; the stored value stays a quaternion
    EntityPicker,
    EnumCombo,
};

const char* to_string(ReflectedWidgetKind kind) noexcept;

/// Widget for a property type. Exposed rather than private so the panel and its
/// tests cannot disagree about the mapping.
ReflectedWidgetKind widget_for(PropertyType type) noexcept;

struct ReflectedField {
    const PropertyInfo* property = nullptr;
    std::string         label;
    std::string         category;
    ReflectedWidgetKind widget = ReflectedWidgetKind::Unsupported;
    /// Variant names in declaration order, for `EnumCombo` only.
    std::vector<std::string> enum_variants;
    /// Cached text form. The widget edits this, then calls `set_value`.
    std::string value;
};

/// A category header and the fields under it, as indices into `fields()` so the
/// panel can look up the property metadata while iterating.
struct ReflectedGroup {
    std::string         category;  // empty for properties that declare none
    std::vector<usize>  field_indices;
};

/// The inspector's view of one reflected object.
///
/// Holds a non-owning pointer to the live object, so it must not outlive it.
/// A view built with a null instance or null metadata is inert — `valid()` is
/// false and every accessor is safe — which is what lets the panel fall back to
/// the hand-written editors for a type that has not opted into reflection.
class ReflectedObjectView {
public:
    ReflectedObjectView() = default;

    /// Collects the object's `EditAnywhere` properties, own and inherited, in
    /// most-derived-first order.
    static ReflectedObjectView build(void* instance, const ClassInfo* meta);

    [[nodiscard]] bool valid() const noexcept { return m_instance != nullptr && m_meta != nullptr; }
    [[nodiscard]] bool empty() const noexcept { return m_fields.empty(); }
    [[nodiscard]] const std::vector<ReflectedField>& fields() const noexcept { return m_fields; }
    [[nodiscard]] const ClassInfo* meta() const noexcept { return m_meta; }

    /// Fields grouped by category, in first-appearance order. A property with no
    /// category lands in the "" group, which the panel draws without a header.
    [[nodiscard]] std::vector<ReflectedGroup> groups() const;

    /// The field's value read straight from the live object. Returns "" for an
    /// out-of-range index.
    [[nodiscard]] std::string value_of(usize index) const;

    /// Writes `text` through to the live object and updates the cached value.
    /// Returns false — leaving the object untouched — for an out-of-range index
    /// or malformed input.
    bool set_value(usize index, std::string_view text);

    /// Re-reads every field from the live object. Call after anything else
    /// changed the object, so the panel does not show stale text.
    void refresh();

private:
    void*            m_instance = nullptr;
    const ClassInfo* m_meta     = nullptr;
    std::vector<ReflectedField> m_fields;
};

/// Registered gameplay module names, sorted. Backs the "Add Gameplay Module"
/// dropdown; sorted because static-init order is unspecified and a menu that
/// reshuffles between runs is a bug report waiting to happen.
std::vector<std::string> gameplay_module_candidates();

} // namespace nf::editor
