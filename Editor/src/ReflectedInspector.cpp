// NF/Editor/ReflectedInspector.cpp — reflection-driven inspector model (Phase 10, W3)

#include <NF/Editor/ReflectedInspector.hpp>

#include <NF/Gameplay/GameplayModuleRegistry.hpp>

#include <algorithm>

namespace nf::editor {

const char* to_string(ReflectedWidgetKind kind) noexcept {
    switch (kind) {
        case ReflectedWidgetKind::Unsupported:    return "Unsupported";
        case ReflectedWidgetKind::FloatDrag:      return "FloatDrag";
        case ReflectedWidgetKind::IntDrag:        return "IntDrag";
        case ReflectedWidgetKind::BoolCheckbox:   return "BoolCheckbox";
        case ReflectedWidgetKind::TextInput:      return "TextInput";
        case ReflectedWidgetKind::Vec3Drag:       return "Vec3Drag";
        case ReflectedWidgetKind::QuatEulerDrag:  return "QuatEulerDrag";
        case ReflectedWidgetKind::EntityPicker:   return "EntityPicker";
        case ReflectedWidgetKind::EnumCombo:      return "EnumCombo";
    }
    return "Unsupported";
}

ReflectedWidgetKind widget_for(PropertyType type) noexcept {
    switch (type) {
        case PropertyType::Float:     return ReflectedWidgetKind::FloatDrag;
        case PropertyType::Int:       return ReflectedWidgetKind::IntDrag;
        case PropertyType::Bool:      return ReflectedWidgetKind::BoolCheckbox;
        case PropertyType::String:    return ReflectedWidgetKind::TextInput;
        case PropertyType::Vec3:      return ReflectedWidgetKind::Vec3Drag;
        case PropertyType::Quat:      return ReflectedWidgetKind::QuatEulerDrag;
        case PropertyType::EntityRef: return ReflectedWidgetKind::EntityPicker;
        case PropertyType::Enum:      return ReflectedWidgetKind::EnumCombo;
        case PropertyType::Array:
        case PropertyType::Unknown:
            break;
    }
    return ReflectedWidgetKind::Unsupported;
}

ReflectedObjectView ReflectedObjectView::build(void* instance, const ClassInfo* meta) {
    ReflectedObjectView view;
    if (instance == nullptr || meta == nullptr) return view;

    view.m_instance = instance;
    view.m_meta = meta;

    std::vector<const PropertyInfo*> properties;
    meta->collect_properties(properties);

    for (const PropertyInfo* property : properties) {
        // `EditAnywhere` is the gate for the inspector, and it is deliberately
        // not the same flag as `SerializeField`: a derived quantity that should
        // be shown but not saved is a normal thing to want.
        if (property == nullptr || property->name == nullptr) continue;
        if (!property->has_flag(Prop_EditAnywhere)) continue;

        ReflectedField field;
        field.property = property;
        field.label    = property->name;
        field.category = property->category != nullptr ? property->category : "";
        field.widget   = widget_for(property->type);
        field.value    = property_to_string(instance, *property);

        if (property->type == PropertyType::Enum && property->enum_name != nullptr) {
            const EnumInfo* info = ReflectionRegistry::instance().find_enum(property->enum_name);
            if (info != nullptr) {
                field.enum_variants.reserve(info->variant_count);
                for (u32 i = 0; i < info->variant_count; ++i) {
                    if (info->variants[i].name != nullptr) {
                        field.enum_variants.emplace_back(info->variants[i].name);
                    }
                }
            }
        }

        view.m_fields.push_back(std::move(field));
    }

    return view;
}

std::vector<ReflectedGroup> ReflectedObjectView::groups() const {
    std::vector<ReflectedGroup> result;

    for (usize i = 0; i < m_fields.size(); ++i) {
        const std::string& category = m_fields[i].category;

        auto it = std::find_if(result.begin(), result.end(),
                               [&category](const ReflectedGroup& g) { return g.category == category; });
        if (it == result.end()) {
            result.push_back(ReflectedGroup{category, {}});
            it = result.end() - 1;
        }
        it->field_indices.push_back(i);
    }

    return result;
}

std::string ReflectedObjectView::value_of(usize index) const {
    if (index >= m_fields.size()) return {};
    const ReflectedField& field = m_fields[index];
    if (field.property == nullptr) return {};
    // Read through rather than returning the cache: the panel calls this to
    // decide whether an edit changed anything, and a stale cache would make an
    // external change look like a no-op.
    return property_to_string(m_instance, *field.property);
}

bool ReflectedObjectView::set_value(usize index, std::string_view text) {
    if (index >= m_fields.size()) return false;
    ReflectedField& field = m_fields[index];
    if (field.property == nullptr) return false;
    if (!property_from_string(m_instance, *field.property, text)) return false;

    field.value = value_of(index);
    return true;
}

void ReflectedObjectView::refresh() {
    for (ReflectedField& field : m_fields) {
        if (field.property == nullptr) continue;
        field.value = property_to_string(m_instance, *field.property);
    }
}

std::vector<std::string> gameplay_module_candidates() {
    return gameplay::GameplayModuleRegistry::instance().names();
}

} // namespace nf::editor
