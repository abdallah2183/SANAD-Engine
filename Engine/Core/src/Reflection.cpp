// NF/Core/Reflection.cpp — reflection registry and value marshalling (Phase 10, W1)

#include <NF/Core/Reflection.hpp>

#include <NF/Core/Math.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nf {

namespace {

// --- Raw member access ------------------------------------------------------
//
// Properties are addressed by byte offset rather than by typed member pointer so
// that one code path can handle every reflected type. The casts are the price of
// that; the offset itself comes from offsetof, so it is exact.

template<typename T>
T* field_at(void* base, const PropertyInfo& prop) {
    return reinterpret_cast<T*>(static_cast<u8*>(base) + prop.offset);
}

template<typename T>
const T* field_at(const void* base, const PropertyInfo& prop) {
    return reinterpret_cast<const T*>(static_cast<const u8*>(base) + prop.offset);
}

/// Reads an enum's underlying bytes as a signed 64-bit value. The width comes
/// from the reflected member, so `enum class : u8` and `: i64` both work without
/// the macro having to name the underlying type.
i64 read_raw_int(const void* base, const PropertyInfo& prop) {
    const void* p = static_cast<const u8*>(base) + prop.offset;
    switch (prop.size) {
        case 1: return static_cast<i64>(*static_cast<const i8*>(p));
        case 2: return static_cast<i64>(*static_cast<const i16*>(p));
        case 4: return static_cast<i64>(*static_cast<const i32*>(p));
        case 8: return *static_cast<const i64*>(p);
        default: return 0;
    }
}

/// Truncates to the destination width. Comparing truncated values is what lets a
/// variant be matched regardless of whether the enum's underlying type is signed
/// — a `u8` variant holding 200 and an `i8` read of the same byte agree here.
i64 truncate_to_width(i64 value, u32 size) {
    switch (size) {
        case 1: return static_cast<i8>(static_cast<u8>(value));
        case 2: return static_cast<i16>(static_cast<u16>(value));
        case 4: return static_cast<i32>(static_cast<u32>(value));
        default: return value;
    }
}

void write_raw_int(void* base, const PropertyInfo& prop, i64 value) {
    void* p = static_cast<u8*>(base) + prop.offset;
    switch (prop.size) {
        case 1: *static_cast<i8*>(p)  = static_cast<i8>(value);  break;
        case 2: *static_cast<i16*>(p) = static_cast<i16>(value); break;
        case 4: *static_cast<i32*>(p) = static_cast<i32>(value); break;
        case 8: *static_cast<i64*>(p) = value;                   break;
        default: break;
    }
}

// --- Text helpers -----------------------------------------------------------

std::string_view skip_space(std::string_view s) {
    usize i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
    return s.substr(i);
}

bool parse_f32(std::string_view text, f32& out) {
    const std::string_view s = skip_space(text);
    if (s.empty()) return false;
    // strtof needs a NUL-terminated buffer; the strings here are short.
    const std::string buf(s);
    char* end = nullptr;
    const float v = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str()) return false;
    out = v;
    return true;
}

bool parse_i64(std::string_view text, i64& out) {
    const std::string_view s = skip_space(text);
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr != first;
}

/// Splits whitespace-separated components, for Vec3 and Quat.
usize split_floats(std::string_view text, f32* out, usize max_count) {
    usize count = 0;
    usize i = 0;
    while (i < text.size() && count < max_count) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == ',')) ++i;
        if (i >= text.size()) break;
        const usize start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != ',') ++i;
        f32 value = 0.0f;
        if (!parse_f32(text.substr(start, i - start), value)) return count;
        out[count++] = value;
    }
    return count;
}

/// Nine significant digits is the shortest width that round-trips every f32
/// exactly. A narrower format is tempting (it reads better) but loses the last
/// bits, which shows up as drift after a save/load cycle.
std::string format_float(f32 value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
    return buf;
}

} // namespace

// --- PropertyType -----------------------------------------------------------

const char* to_string(PropertyType type) noexcept {
    switch (type) {
        case PropertyType::Unknown:   return "Unknown";
        case PropertyType::Float:     return "Float";
        case PropertyType::Int:       return "Int";
        case PropertyType::Bool:      return "Bool";
        case PropertyType::String:    return "String";
        case PropertyType::Vec3:      return "Vec3";
        case PropertyType::Quat:      return "Quat";
        case PropertyType::EntityRef: return "EntityRef";
        case PropertyType::Enum:      return "Enum";
        case PropertyType::Array:     return "Array";
    }
    return "Unknown";
}

// --- ClassInfo --------------------------------------------------------------

const PropertyInfo* ClassInfo::find_property(std::string_view prop) const noexcept {
    for (const ClassInfo* c = this; c != nullptr; c = c->parent) {
        if (c->properties == nullptr) continue;
        for (u32 i = 0; i < c->property_count; ++i) {
            const PropertyInfo& candidate = c->properties[i];
            if (candidate.name != nullptr && prop == candidate.name) return &candidate;
        }
    }
    return nullptr;
}

u32 ClassInfo::total_property_count() const noexcept {
    u32 total = 0;
    for (const ClassInfo* c = this; c != nullptr; c = c->parent) {
        total += c->property_count;
    }
    return total;
}

bool ClassInfo::is_a(const ClassInfo* other) const noexcept {
    for (const ClassInfo* c = this; c != nullptr; c = c->parent) {
        if (c == other) return true;
    }
    return false;
}

void ClassInfo::collect_properties(std::vector<const PropertyInfo*>& out) const {
    for (const ClassInfo* c = this; c != nullptr; c = c->parent) {
        if (c->properties == nullptr) continue;
        for (u32 i = 0; i < c->property_count; ++i) {
            out.push_back(&c->properties[i]);
        }
    }
}

// --- EnumInfo ---------------------------------------------------------------

const EnumInfo::Variant* EnumInfo::find_variant(std::string_view variant) const noexcept {
    if (variants == nullptr) return nullptr;
    for (u32 i = 0; i < variant_count; ++i) {
        if (variants[i].name != nullptr && variant == variants[i].name) return &variants[i];
    }
    return nullptr;
}

const EnumInfo::Variant* EnumInfo::find_value(i64 value) const noexcept {
    if (variants == nullptr) return nullptr;
    for (u32 i = 0; i < variant_count; ++i) {
        if (variants[i].value == value) return &variants[i];
    }
    return nullptr;
}

// --- ReflectionRegistry -----------------------------------------------------

ReflectionRegistry& ReflectionRegistry::instance() noexcept {
    static ReflectionRegistry registry;
    return registry;
}

void ReflectionRegistry::register_class(const ClassInfo* info) {
    if (info == nullptr || info->name == nullptr) return;
    // A header included in several TUs still has one ClassInfo (the accessor's
    // function-local static), so identity is usually enough — the name check is
    // for the case where two TUs each got their own copy.
    for (const ClassInfo* existing : m_classes) {
        if (existing == info) return;
        if (std::string_view(existing->name) == info->name) return;
    }
    m_classes.push_back(info);
}

void ReflectionRegistry::register_enum(const EnumInfo* info) {
    if (info == nullptr || info->name == nullptr) return;
    for (const EnumInfo* existing : m_enums) {
        if (existing == info) return;
        if (std::string_view(existing->name) == info->name) return;
    }
    m_enums.push_back(info);
}

const ClassInfo* ReflectionRegistry::find_class(std::string_view name) const noexcept {
    for (const ClassInfo* info : m_classes) {
        if (info->name != nullptr && name == info->name) return info;
    }
    return nullptr;
}

const EnumInfo* ReflectionRegistry::find_enum(std::string_view name) const noexcept {
    for (const EnumInfo* info : m_enums) {
        if (info->name != nullptr && name == info->name) return info;
    }
    return nullptr;
}

// --- Marshalling ------------------------------------------------------------

std::string property_to_string(const void* instance, const PropertyInfo& prop) {
    if (instance == nullptr || prop.name == nullptr) return {};

    switch (prop.type) {
        case PropertyType::Float:
            return format_float(*field_at<f32>(instance, prop));

        case PropertyType::Int:
            return std::to_string(*field_at<i32>(instance, prop));

        case PropertyType::Bool:
            return *field_at<bool>(instance, prop) ? "true" : "false";

        case PropertyType::String:
            return *field_at<std::string>(instance, prop);

        case PropertyType::Vec3: {
            const Vec3 v = *field_at<Vec3>(instance, prop);
            return format_float(v.x) + " " + format_float(v.y) + " " + format_float(v.z);
        }

        case PropertyType::Quat: {
            const Quat q = *field_at<Quat>(instance, prop);
            return format_float(q.x) + " " + format_float(q.y) + " " +
                   format_float(q.z) + " " + format_float(q.w);
        }

        case PropertyType::EntityRef: {
            const u32 id = *field_at<u32>(instance, prop);
            return id == u32_max ? std::string("none") : std::to_string(id);
        }

        case PropertyType::Enum: {
            const i64 raw = truncate_to_width(read_raw_int(instance, prop), prop.size);
            if (prop.enum_name != nullptr) {
                const EnumInfo* info = ReflectionRegistry::instance().find_enum(prop.enum_name);
                if (info != nullptr) {
                    for (u32 i = 0; i < info->variant_count; ++i) {
                        if (truncate_to_width(info->variants[i].value, prop.size) == raw) {
                            return info->variants[i].name;
                        }
                    }
                }
            }
            // Unregistered enum or an out-of-range value: the number still round-
            // trips, which matters more than the name being pretty.
            return std::to_string(raw);
        }

        case PropertyType::Array:
        case PropertyType::Unknown:
            break;
    }
    return {};
}

bool property_from_string(void* instance, const PropertyInfo& prop, std::string_view text) {
    if (instance == nullptr || prop.name == nullptr) return false;

    switch (prop.type) {
        case PropertyType::Float: {
            f32 value = 0.0f;
            if (!parse_f32(text, value)) return false;
            *field_at<f32>(instance, prop) = value;
            return true;
        }

        case PropertyType::Int: {
            i64 value = 0;
            if (!parse_i64(text, value)) return false;
            *field_at<i32>(instance, prop) = static_cast<i32>(value);
            return true;
        }

        case PropertyType::Bool: {
            const std::string_view s = skip_space(text);
            if (s == "true" || s == "1") {
                *field_at<bool>(instance, prop) = true;
                return true;
            }
            if (s == "false" || s == "0") {
                *field_at<bool>(instance, prop) = false;
                return true;
            }
            return false;
        }

        case PropertyType::String:
            // Not trimmed: a string property is allowed to carry its own spaces.
            *field_at<std::string>(instance, prop) = std::string(text);
            return true;

        case PropertyType::Vec3: {
            f32 v[3] = {0, 0, 0};
            if (split_floats(text, v, 3) != 3) return false;
            *field_at<Vec3>(instance, prop) = Vec3{v[0], v[1], v[2]};
            return true;
        }

        case PropertyType::Quat: {
            f32 v[4] = {0, 0, 0, 1};
            if (split_floats(text, v, 4) != 4) return false;
            *field_at<Quat>(instance, prop) = Quat{v[0], v[1], v[2], v[3]};
            return true;
        }

        case PropertyType::EntityRef: {
            const std::string_view s = skip_space(text);
            if (s == "none") {
                *field_at<u32>(instance, prop) = u32_max;
                return true;
            }
            i64 value = 0;
            if (!parse_i64(s, value)) return false;
            *field_at<u32>(instance, prop) = static_cast<u32>(value);
            return true;
        }

        case PropertyType::Enum: {
            const std::string_view s = skip_space(text);
            if (prop.enum_name != nullptr) {
                const EnumInfo* info = ReflectionRegistry::instance().find_enum(prop.enum_name);
                if (info != nullptr) {
                    if (const EnumInfo::Variant* variant = info->find_variant(s)) {
                        write_raw_int(instance, prop, variant->value);
                        return true;
                    }
                }
            }
            i64 value = 0;
            if (!parse_i64(s, value)) return false;
            write_raw_int(instance, prop, value);
            return true;
        }

        case PropertyType::Array:
        case PropertyType::Unknown:
            break;
    }
    return false;
}

} // namespace nf
