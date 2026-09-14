// NF/Rendering/MaterialAsset.cpp — .nfmat text parse/serialize (see header).

#include <NF/Rendering/MaterialAsset.hpp>

#include <cctype>
#include <cstdio>
#include <sstream>

namespace nf::rendering {

namespace {

std::string trim_str(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

bool parse_floats(const std::string& s, float* out, int count) {
    std::istringstream iss(s);
    for (int i = 0; i < count; ++i) {
        if (!(iss >> out[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool MaterialAsset::load_from_text(const std::string& text, MaterialAsset& out, std::string& out_err) {
    MaterialAsset parsed;
    std::istringstream iss(text);
    std::string line;
    if (!std::getline(iss, line) || trim_str(line).rfind("# NOVAForge Material", 0) != 0) {
        out_err = "Not a .nfmat file (missing '# NOVAForge Material' header)";
        return false;
    }
    while (std::getline(iss, line)) {
        line = trim_str(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trim_str(line.substr(0, colon));
        const std::string val = trim_str(line.substr(colon + 1));
        if (key == "name") {
            if (!val.empty()) {
                parsed.name = val;
            }
        } else if (key == "base_color") {
            float v[4]{};
            if (parse_floats(val, v, 4)) {
                for (int i = 0; i < 4; ++i) {
                    parsed.params.base_color[i] = v[i];
                }
            }
        } else if (key == "metallic") {
            float v = 0.0f;
            if (parse_floats(val, &v, 1)) {
                parsed.params.metallic = v;
            }
        } else if (key == "roughness") {
            float v = 0.0f;
            if (parse_floats(val, &v, 1)) {
                parsed.params.roughness = v;
            }
        } else if (key == "ao") {
            float v = 0.0f;
            if (parse_floats(val, &v, 1)) {
                parsed.params.ao = v;
            }
        } else if (key == "emission") {
            float v[3]{};
            if (parse_floats(val, v, 3)) {
                for (int i = 0; i < 3; ++i) {
                    parsed.params.emission[i] = v[i];
                }
            }
        } else if (key == "emission_strength") {
            float v = 0.0f;
            if (parse_floats(val, &v, 1)) {
                parsed.params.emission_strength = v;
            }
        } else if (key == "albedo") {
            parsed.albedo = val;
        } else if (key == "mip") {
            if (val == "none") {
                parsed.mip_mode = rhi::MipMapMode::None;
            } else if (val == "nearest") {
                parsed.mip_mode = rhi::MipMapMode::Nearest;
            } else if (val == "linear") {
                parsed.mip_mode = rhi::MipMapMode::Linear;
            }
            // Unknown values keep the default (tolerant, like every key).
        }
        // Unknown keys are ignored: forward compatibility.
    }
    out = parsed;
    out_err.clear();
    return true;
}

std::string MaterialAsset::save_to_text() const {
    char buf[1024];
    std::string out = "# NOVAForge Material v1\n";
    out += "name: " + name + "\n";
    std::snprintf(buf, sizeof(buf), "base_color: %g %g %g %g\n", static_cast<double>(params.base_color[0]),
                  static_cast<double>(params.base_color[1]), static_cast<double>(params.base_color[2]),
                  static_cast<double>(params.base_color[3]));
    out += buf;
    std::snprintf(buf, sizeof(buf), "metallic: %g\n", static_cast<double>(params.metallic));
    out += buf;
    std::snprintf(buf, sizeof(buf), "roughness: %g\n", static_cast<double>(params.roughness));
    out += buf;
    std::snprintf(buf, sizeof(buf), "ao: %g\n", static_cast<double>(params.ao));
    out += buf;
    std::snprintf(buf, sizeof(buf), "emission: %g %g %g\n", static_cast<double>(params.emission[0]),
                  static_cast<double>(params.emission[1]), static_cast<double>(params.emission[2]));
    out += buf;
    std::snprintf(buf, sizeof(buf), "emission_strength: %g\n",
                  static_cast<double>(params.emission_strength));
    out += buf;
    if (!albedo.empty()) {
        out += "albedo: " + albedo + "\n";
    }
    out += std::string("mip: ") +
           (mip_mode == rhi::MipMapMode::None
                ? "none"
                : (mip_mode == rhi::MipMapMode::Nearest ? "nearest" : "linear")) +
           "\n";
    return out;
}

} // namespace nf::rendering
