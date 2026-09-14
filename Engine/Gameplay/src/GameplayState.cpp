// NF/Gameplay/GameplayState.cpp — module state <-> text map (Phase 10, W2)

#include <NF/Gameplay/GameplayState.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

namespace nf::gameplay {

namespace {

std::string escape_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|':  out += "\\p";  break;
            case ' ':  out += "\\s";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string unescape_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (usize i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out += value[i];
            continue;
        }
        switch (value[++i]) {
            case '\\': out += '\\'; break;
            case 'p':  out += '|';  break;
            case 's':  out += ' ';  break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            // Unknown escape kept verbatim. Dropping the backslash would corrupt
            // a value that a newer engine wrote.
            default:   out += '\\'; out += value[i]; break;
        }
    }
    return out;
}

/// Reflected properties that belong in a save file, most-derived first and
/// deduplicated.
///
/// The dedupe matters: `collect_properties` walks the inheritance chain, and a
/// derived class that shadows a base property of the same name would otherwise
/// be visited twice — the base entry would then overwrite the derived one in the
/// map, which is backwards.
std::vector<const PropertyInfo*> serializable_properties(const GameplayStateBinding& binding) {
    std::vector<const PropertyInfo*> all;
    binding.meta->collect_properties(all);

    std::vector<const PropertyInfo*> filtered;
    filtered.reserve(all.size());
    for (const PropertyInfo* prop : all) {
        if (prop->name == nullptr || !prop->has_flag(Prop_SerializeField)) continue;
        const bool already_seen =
            std::any_of(filtered.begin(), filtered.end(), [prop](const PropertyInfo* seen) {
                return std::string_view(seen->name) == prop->name;
            });
        if (!already_seen) filtered.push_back(prop);
    }
    return filtered;
}

} // namespace

bool capture_state(const GameplayStateBinding& binding,
                   std::unordered_map<std::string, std::string>& out) {
    out.clear();
    if (!binding.valid()) return false;

    for (const PropertyInfo* prop : serializable_properties(binding)) {
        out.emplace(prop->name, property_to_string(binding.instance, *prop));
    }
    return true;
}

u32 apply_state(const GameplayStateBinding& binding,
                const std::unordered_map<std::string, std::string>& in) {
    if (!binding.valid()) return 0;

    u32 applied = 0;
    for (const PropertyInfo* prop : serializable_properties(binding)) {
        const auto it = in.find(prop->name);
        if (it == in.end()) continue;
        if (property_from_string(binding.instance, *prop, it->second)) ++applied;
    }
    return applied;
}

std::string encode_properties(const std::unordered_map<std::string, std::string>& properties) {
    std::string out;
    for (const auto& [key, value] : properties) {
        if (!out.empty()) out += '|';
        out += key;
        out += '=';
        out += escape_value(value);
    }
    return out;
}

void decode_properties(std::string_view encoded,
                       std::unordered_map<std::string, std::string>& out) {
    usize start = 0;
    while (start <= encoded.size()) {
        const usize sep = encoded.find('|', start);
        const std::string_view pair =
            encoded.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
        if (!pair.empty()) {
            const usize eq = pair.find('=');
            if (eq != std::string_view::npos) {
                out.emplace(std::string(pair.substr(0, eq)), unescape_value(pair.substr(eq + 1)));
            }
        }
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
}

} // namespace nf::gameplay
