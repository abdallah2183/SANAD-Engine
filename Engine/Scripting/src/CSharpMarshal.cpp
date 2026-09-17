// NF/Scripting/CSharpMarshal.cpp — interop helpers (pure logic, no runtime).

#include <NF/Scripting/CSharpMarshal.hpp>

namespace nf::scripting {

long long write_string_to_buffer(std::string_view text, u8* out, usize capacity) {
    if (text.empty()) return 0;
    if (out == nullptr) return -1;
    if (text.size() > capacity) return -1;
    for (usize i = 0; i < text.size(); ++i) {
        out[i] = static_cast<u8>(text[i]);
    }
    return static_cast<long long>(text.size());
}

std::string csharp_type_name(std::string_view assembly, std::string_view dotted_type) {
    std::string out(dotted_type);
    out += ", ";
    out += assembly;
    return out;
}

} // namespace nf::scripting
