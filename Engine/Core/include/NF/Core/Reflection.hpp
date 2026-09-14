#pragma once

// NF/Core/Reflection.hpp — compile-time property metadata (Phase 10, W1)
//
// The design doc's §249 S1 asks for properties that can be discovered, edited in
// the inspector, and serialized without hand-written boilerplate. This is the
// metadata layer that makes that possible; it deliberately has no codegen step
// (unlike Unreal's UHT), so a property declaration is one macro next to the
// member it describes and nothing else has to be kept in sync.
//
// Scope note: this is *reflection*, not a scripting VM. It records where a
// property lives and what it is; it does not create bindings, and there is no
// runtime type construction. S2+ (C#/Lua, hot reload, visual scripting) is a
// later phase and this header is not the seam for it — the GameplayModule layer
// is.
//
// Usage — the three macros all live inside the class body:
//
//     struct OrbitState {
//         f32 radius = 5.0f;
//         i32 steps  = 0;
//
//         NF_CLASS(OrbitState)
//         NF_PROPERTY(OrbitState, radius, Float, Prop_EditAnywhere, "Orbit")
//         NF_PROPERTY(OrbitState, steps,  Int,   Prop_EditAnywhere | Prop_SerializeField, "Orbit")
//         NF_CLASS_END(OrbitState)
//     };
//
// `NF_CLASS` opens a static function whose body holds the property table, so the
// entries are evaluated in a complete-class context — `offsetof` needs a complete
// type, and the class is only complete inside a member function body. `NF_CLASS_END`
// closes it and adds `nf_class_meta()` plus a static-init registrar, which is what
// makes a class discoverable without ever instantiating it.
//
// Because the macros declare members, the type must be a `struct` (or have a
// `public:` before `NF_CLASS`).

#include <NF/Core/Types.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nf {

// --- Property type tags -----------------------------------------------------
// A closed set on purpose. Every value here has a widget in the inspector and a
// text form in the save format; adding a type means adding both.
enum class PropertyType : u8 {
    Unknown = 0,
    Float,      // f32
    Int,        // i32
    Bool,       // bool
    String,     // std::string
    Vec3,       // nf::Vec3
    Quat,       // nf::Quat
    EntityRef,  // u32 entity id (weak reference — generation is not carried)
    Enum,       // any enum class; `enum_name` names the EnumInfo
    Array,      // reserved; no reflected array type exists yet
};

/// Stable, human-readable name for a property type. Used by the inspector and by
/// the save format's diagnostics.
const char* to_string(PropertyType type) noexcept;

enum PropertyFlags : u32 {
    Prop_None           = 0u,
    Prop_EditAnywhere   = 1u << 0,  ///< show in the inspector
    Prop_SerializeField = 1u << 1,  ///< write to the save file
    Prop_Replicated     = 1u << 2,  ///< networking (v0.4) — currently a no-op tag
};

struct PropertyInfo {
    const char*  name      = nullptr;
    PropertyType type      = PropertyType::Unknown;
    u32          offset    = 0;  ///< bytes from the object base
    u32          size      = 0;  ///< sizeof the member, for raw reads/writes
    u32          flags     = Prop_None;
    const char*  category  = nullptr;  ///< inspector group header
    const char*  enum_name = nullptr;  ///< non-null only when type == Enum

    [[nodiscard]] bool has_flag(PropertyFlags flag) const noexcept {
        return (flags & static_cast<u32>(flag)) != 0u;
    }
};

struct ClassInfo {
    const char*         name           = nullptr;
    const ClassInfo*    parent         = nullptr;
    const PropertyInfo* properties     = nullptr;
    u32                 property_count = 0;

    /// Nearest declaration wins, so a derived class can shadow a base property.
    [[nodiscard]] const PropertyInfo* find_property(std::string_view prop) const noexcept;

    /// This class plus every ancestor, walked to the root.
    [[nodiscard]] u32 total_property_count() const noexcept;

    [[nodiscard]] bool is_a(const ClassInfo* other) const noexcept;

    /// Copies every reflected property (own and inherited, most-derived first)
    /// into `out`. Used by the inspector, which must show inherited members.
    void collect_properties(std::vector<const PropertyInfo*>& out) const;
};

struct EnumInfo {
    struct Variant {
        const char* name  = nullptr;
        i64         value = 0;
    };

    const char*    name          = nullptr;
    const Variant* variants      = nullptr;
    u32            variant_count = 0;

    [[nodiscard]] const Variant* find_variant(std::string_view variant) const noexcept;
    [[nodiscard]] const Variant* find_value(i64 value) const noexcept;
};

/// Process-wide registry. Registration happens during static initialisation, so
/// the editor can enumerate classes it has never instantiated. A function-local
/// static keeps the registry itself immune to cross-TU init order.
class ReflectionRegistry {
public:
    static ReflectionRegistry& instance() noexcept;

    void register_class(const ClassInfo* info);
    void register_enum(const EnumInfo* info);

    [[nodiscard]] const ClassInfo* find_class(std::string_view name) const noexcept;
    [[nodiscard]] const EnumInfo*  find_enum(std::string_view name) const noexcept;

    [[nodiscard]] const std::vector<const ClassInfo*>& classes() const noexcept { return m_classes; }
    [[nodiscard]] const std::vector<const EnumInfo*>&  enums() const noexcept { return m_enums; }

    // No clear(). Registration happens during static initialisation and cannot
    // be re-run, so a reset would permanently disable reflection for the rest of
    // the process — a test that used it would silently break every test after
    // it. Tests assert on presence by name instead.

private:
    std::vector<const ClassInfo*> m_classes;
    std::vector<const EnumInfo*>  m_enums;
};

// --- Value marshalling ------------------------------------------------------
//
// The text form is both the save format and the inspector's edit buffer, so the
// two cannot drift: both go through here. Returns an empty string for a null
// instance or an Unknown property rather than asserting — a missing property is
// a data problem, not a programming error.
std::string property_to_string(const void* instance, const PropertyInfo& prop);

/// Parses `text` into the property. Returns false and leaves the value untouched
/// on malformed input, so a corrupt save file cannot half-apply.
bool property_from_string(void* instance, const PropertyInfo& prop, std::string_view text);

// --- Registration helpers ---------------------------------------------------
template<typename T>
struct ClassRegistrar {
    explicit ClassRegistrar(const ClassInfo* info) {
        ReflectionRegistry::instance().register_class(info);
    }
};

struct EnumRegistrar {
    explicit EnumRegistrar(const EnumInfo* info) {
        ReflectionRegistry::instance().register_enum(info);
    }
};

namespace detail {

template<typename T>
ClassInfo make_class_info(const char* name, const ClassInfo* parent) {
    ClassInfo info{};
    info.name       = name;
    info.parent     = parent;
    info.properties = T::nf_reflect_props(info.property_count);
    return info;
}

} // namespace detail

} // namespace nf

// ---------------------------------------------------------------------------
// Class / property macros
// ---------------------------------------------------------------------------

#define NF_CLASS(Type)                                                        \
    static const ::nf::PropertyInfo* nf_reflect_props(::nf::u32& nf_reflect_count) { \
        static const ::nf::PropertyInfo nf_reflect_table[] = {

#define NF_PROPERTY(Type, member, ptype, pflags, pcat)                        \
            ::nf::PropertyInfo{ #member, ::nf::PropertyType::ptype,           \
                static_cast<::nf::u32>(offsetof(Type, member)),               \
                static_cast<::nf::u32>(sizeof(decltype(Type::member))),       \
                static_cast<::nf::u32>(pflags), pcat, nullptr },

#define NF_PROPERTY_ENUM(Type, member, ptype, pflags, pcat, penum)            \
            ::nf::PropertyInfo{ #member, ::nf::PropertyType::ptype,           \
                static_cast<::nf::u32>(offsetof(Type, member)),               \
                static_cast<::nf::u32>(sizeof(decltype(Type::member))),       \
                static_cast<::nf::u32>(pflags), pcat, penum },

// Root class. The registrar's initialiser calls nf_class_meta(), so simply
// including the header is enough to make the class discoverable.
#define NF_CLASS_END(Type)                                                    \
        };                                                                    \
        nf_reflect_count = static_cast<::nf::u32>(                            \
            sizeof(nf_reflect_table) / sizeof(nf_reflect_table[0]));          \
        return nf_reflect_table;                                              \
    }                                                                         \
    static const ::nf::ClassInfo* nf_class_meta() {                           \
        static const ::nf::ClassInfo nf_info =                                \
            ::nf::detail::make_class_info<Type>(#Type, nullptr);              \
        return &nf_info;                                                      \
    }                                                                         \
    inline static const ::nf::ClassRegistrar<Type> nf_class_registrar{        \
        Type::nf_class_meta() };

// Derived class. `Parent` must itself have been declared with NF_CLASS_END.
#define NF_CLASS_END_DERIVED(Type, Parent)                                    \
        };                                                                    \
        nf_reflect_count = static_cast<::nf::u32>(                            \
            sizeof(nf_reflect_table) / sizeof(nf_reflect_table[0]));          \
        return nf_reflect_table;                                              \
    }                                                                         \
    static const ::nf::ClassInfo* nf_class_meta() {                           \
        static const ::nf::ClassInfo nf_info =                                \
            ::nf::detail::make_class_info<Type>(#Type, Parent::nf_class_meta()); \
        return &nf_info;                                                      \
    }                                                                         \
    inline static const ::nf::ClassRegistrar<Type> nf_class_registrar{        \
        Type::nf_class_meta() };

// ---------------------------------------------------------------------------
// Enum macro
// ---------------------------------------------------------------------------
//
//     enum class Mode : u8 { Off, Slow, Fast };
//
//     NF_ENUM_BEGIN(Mode)
//         NF_ENUM_VALUE(Mode, Off)
//         NF_ENUM_VALUE(Mode, Slow)
//         NF_ENUM_VALUE(Mode, Fast)
//     NF_ENUM_END(Mode, "Mode")
//
// `inline` variables give one definition across TUs; the registrar then fires
// exactly once even though the header is included everywhere.

#define NF_ENUM_BEGIN(Type)                                                   \
    namespace nf_enum_registry {                                              \
    [[maybe_unused]] inline const ::nf::EnumInfo::Variant nf_enum_variants_##Type[] = {

#define NF_ENUM_VALUE(Type, Value)                                            \
        ::nf::EnumInfo::Variant{ #Value, static_cast<::nf::i64>(Type::Value) },

#define NF_ENUM_END(Type, Name)                                               \
    };                                                                        \
    [[maybe_unused]] inline const ::nf::EnumInfo nf_enum_info_##Type{         \
        Name, nf_enum_variants_##Type,                                        \
        static_cast<::nf::u32>(sizeof(nf_enum_variants_##Type) /              \
                               sizeof(nf_enum_variants_##Type[0])) };         \
    [[maybe_unused]] inline const ::nf::EnumRegistrar nf_enum_registrar_##Type{ \
        &nf_enum_info_##Type };                                               \
    } // namespace nf_enum_registry
