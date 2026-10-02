// NF/Assets/MeshImport.cpp — readers for every mesh format the engine accepts.
//
// MeshImport.hpp holds the contract and the format table; this file is the
// implementation. Structure, top to bottom:
//   shared geometry helpers   — bounds + smooth normals, used by every reader
//   byte/text scanning        — line and token scanners the text formats share
//   .nfmesh / glTF            — pass-through and delegate
//   Wavefront OBJ             — groups, material-slot runs, index-triple dedupe
//   STL                       — binary and ASCII, triangle soup -> indexed mesh
//   Stanford PLY              — ASCII and binary, both endiannesses
//   dispatch                  — extension first, content sniffing as fallback
//
// The readers are deliberately strict: a malformed face is counted in
// faces_skipped and left out, never guessed at, and every format-specific loss
// (STL's missing normals, OBJ's single UV channel) is named in `warnings`.

#include <NF/Assets/MeshImport.hpp>

#include <NF/Assets/GltfImport.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace nf::assets {

// ---------------------------------------------------------------------------
// Shared geometry helpers
// ---------------------------------------------------------------------------

void recompute_mesh_bounds(MeshAsset& mesh) {
    mesh.bounds = AssetAABB{};
    mesh.sphere = AssetSphere{};
    if (!mesh.vertices.empty()) {
        const float* first = mesh.vertices[0].position;
        AssetAABB box;
        box.min_x = box.max_x = first[0];
        box.min_y = box.max_y = first[1];
        box.min_z = box.max_z = first[2];
        for (const AssetVertex& v : mesh.vertices) {
            const float* p = v.position;
            if (p[0] < box.min_x) box.min_x = p[0];
            if (p[0] > box.max_x) box.max_x = p[0];
            if (p[1] < box.min_y) box.min_y = p[1];
            if (p[1] > box.max_y) box.max_y = p[1];
            if (p[2] < box.min_z) box.min_z = p[2];
            if (p[2] > box.max_z) box.max_z = p[2];
        }

        const float cx = (box.min_x + box.max_x) * 0.5f;
        const float cy = (box.min_y + box.max_y) * 0.5f;
        const float cz = (box.min_z + box.max_z) * 0.5f;
        float radius_sq = 0.0f;
        for (const AssetVertex& v : mesh.vertices) {
            const float dx = v.position[0] - cx;
            const float dy = v.position[1] - cy;
            const float dz = v.position[2] - cz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > radius_sq) radius_sq = d2;
        }
        mesh.bounds = box;
        mesh.sphere = AssetSphere{cx, cy, cz, std::sqrt(radius_sq)};
    }

    for (AssetSubMesh& sub : mesh.submeshes) {
        sub.bounds = mesh.bounds;
        sub.sphere = mesh.sphere;
    }
}

void compute_smooth_normals(std::vector<AssetVertex>& vertices, const std::vector<u32>& indices,
                            usize base_vertex, usize index_start, usize index_count) {
    const usize end = index_start + index_count;
    for (usize i = index_start; i + 2 < end; i += 3) {
        const u32 a = indices[i];
        const u32 b = indices[i + 1];
        const u32 c = indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
        const float* pa = vertices[a].position;
        const float* pb = vertices[b].position;
        const float* pc = vertices[c].position;
        const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
        const float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        vertices[a].normal[0] += nx;
        vertices[a].normal[1] += ny;
        vertices[a].normal[2] += nz;
        vertices[b].normal[0] += nx;
        vertices[b].normal[1] += ny;
        vertices[b].normal[2] += nz;
        vertices[c].normal[0] += nx;
        vertices[c].normal[1] += ny;
        vertices[c].normal[2] += nz;
    }
    for (usize i = base_vertex; i < vertices.size(); ++i) {
        float* n = vertices[i].normal;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        } else {
            n[0] = 0.0f;
            n[1] = 0.0f;
            n[2] = 1.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Format table
// ---------------------------------------------------------------------------

namespace {

/// A vertex or index count above this is treated as a corrupt header rather
/// than as a request to allocate gigabytes. The same ceiling GltfImport uses.
constexpr usize kAbsurdCount = 100000000;

std::string lowercase_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string stem_of(const std::string& path) {
    std::string s = std::filesystem::path(path).stem().string();
    if (s.empty()) s = "mesh";
    return s;
}

bool read_all_bytes(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        err = "cannot open '" + path + "'";
        return false;
    }
    const std::streamoff len = in.tellg();
    if (len <= 0) {
        err = "file is empty: '" + path + "'";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(len));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!in) {
        err = "short read on '" + path + "'";
        return false;
    }
    return true;
}

// --- byte/text scanning -----------------------------------------------------

/// Whitespace-separated tokens over a byte range. Used by the ASCII forms of
/// STL and PLY, where the property order is known and a token stream is enough.
class TokenScanner {
public:
    explicit TokenScanner(std::span<const uint8_t> bytes, usize start = 0)
        : m_bytes(bytes), m_pos(start) {}

    void skip_whitespace() {
        while (m_pos < m_bytes.size() && std::isspace(static_cast<unsigned char>(m_bytes[m_pos]))) {
            ++m_pos;
        }
    }

    bool at_end() {
        skip_whitespace();
        return m_pos >= m_bytes.size();
    }

    std::string_view next() {
        skip_whitespace();
        const usize begin = m_pos;
        while (m_pos < m_bytes.size() && !std::isspace(static_cast<unsigned char>(m_bytes[m_pos]))) {
            ++m_pos;
        }
        return std::string_view(reinterpret_cast<const char*>(m_bytes.data() + begin), m_pos - begin);
    }

    bool next_float(float& out) {
        const std::string_view tok = next();
        if (tok.empty()) return false;
        const std::string tmp(tok);
        char* end = nullptr;
        const double v = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        out = static_cast<float>(v);
        return true;
    }

    bool next_double(double& out) {
        const std::string_view tok = next();
        if (tok.empty()) return false;
        const std::string tmp(tok);
        char* end = nullptr;
        const double v = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        out = v;
        return true;
    }

    bool next_long(long& out) {
        const std::string_view tok = next();
        if (tok.empty()) return false;
        const std::string tmp(tok);
        char* end = nullptr;
        const long v = std::strtol(tmp.c_str(), &end, 10);
        if (end == tmp.c_str()) return false;
        out = v;
        return true;
    }

    usize position() const { return m_pos; }

private:
    std::span<const uint8_t> m_bytes;
    usize m_pos = 0;
};

/// Line scanner for the formats that are line-structured (OBJ, and the PLY
/// header). Handles LF and CRLF; the trailing '\r' never reaches a caller.
class LineScanner {
public:
    explicit LineScanner(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

    bool next(std::string_view& out) {
        if (m_pos >= m_bytes.size()) return false;
        const usize begin = m_pos;
        while (m_pos < m_bytes.size() && m_bytes[m_pos] != '\n') ++m_pos;
        usize end = m_pos;
        if (m_pos < m_bytes.size()) ++m_pos; // consume the '\n'
        if (end > begin && m_bytes[end - 1] == '\r') --end;
        out = std::string_view(reinterpret_cast<const char*>(m_bytes.data() + begin), end - begin);
        return true;
    }

    usize byte_offset_after_last_line() const { return m_pos; }

private:
    std::span<const uint8_t> m_bytes;
    usize m_pos = 0;
};

void split_tokens(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const usize begin = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > begin) out.push_back(line.substr(begin, i - begin));
    }
}

std::string_view strip_comment(std::string_view line) {
    const usize hash = line.find('#');
    return hash == std::string_view::npos ? line : line.substr(0, hash);
}

bool parse_float(std::string_view tok, float& out) {
    if (tok.empty()) return false;
    const std::string tmp(tok);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    out = static_cast<float>(v);
    return true;
}

bool finite_vec3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// --- STL signature tests ----------------------------------------------------

bool looks_like_binary_stl(std::span<const uint8_t> bytes) {
    if (bytes.size() < 84) return false;
    u32 count = 0;
    std::memcpy(&count, bytes.data() + 80, sizeof(count));
    if (count > kAbsurdCount) return false;
    return 84u + static_cast<usize>(count) * 50u == bytes.size();
}

bool starts_with_solid_token(std::span<const uint8_t> bytes) {
    if (bytes.size() < 5) return false;
    if (std::memcmp(bytes.data(), "solid", 5) != 0) return false;
    return bytes.size() == 5 || std::isspace(static_cast<unsigned char>(bytes[5])) != 0;
}

// --- binary scalar reads (PLY) ---------------------------------------------

u16 bswap16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }

u32 bswap32(u32 v) {
    return (v >> 24) | ((v & 0x00FF0000u) >> 8) | ((v & 0x0000FF00u) << 8) | (v << 24);
}

u64 bswap64(u64 v) {
    return (v >> 56) | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x000000000000FF00ull) << 40) | (v << 56);
}

} // namespace

const std::vector<MeshImportFormat>& mesh_import_formats() {
    // Mirrors MeshExport's mesh_formats() order on purpose: the Import and
    // Export dialogs list the same six containers in the same order, so the two
    // menus describe one mental model instead of two.
    static const std::vector<MeshImportFormat> kFormats = {
        MeshImportFormat::NfMesh, MeshImportFormat::Obj,  MeshImportFormat::Stl,
        MeshImportFormat::Ply,    MeshImportFormat::Gltf, MeshImportFormat::Glb,
    };
    return kFormats;
}

const char* mesh_import_format_name(MeshImportFormat format) {
    switch (format) {
        case MeshImportFormat::NfMesh: return "NOVAForge Mesh";
        case MeshImportFormat::Obj: return "Wavefront OBJ";
        case MeshImportFormat::Stl: return "STL (binary or ASCII)";
        case MeshImportFormat::Ply: return "Stanford PLY";
        case MeshImportFormat::Gltf: return "glTF 2.0 (JSON)";
        case MeshImportFormat::Glb: return "glTF 2.0 (binary)";
        default: return "unknown";
    }
}

const char* mesh_import_format_extension(MeshImportFormat format) {
    switch (format) {
        case MeshImportFormat::NfMesh: return "nfmesh";
        case MeshImportFormat::Obj: return "obj";
        case MeshImportFormat::Stl: return "stl";
        case MeshImportFormat::Ply: return "ply";
        case MeshImportFormat::Gltf: return "gltf";
        case MeshImportFormat::Glb: return "glb";
        default: return "";
    }
}

bool mesh_import_format_from_extension(std::string_view extension, MeshImportFormat& out) {
    std::string ext(extension);
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (MeshImportFormat f : mesh_import_formats()) {
        if (ext == mesh_import_format_extension(f)) {
            out = f;
            return true;
        }
    }
    return false;
}

MeshImportFormat mesh_import_format_family(MeshImportFormat format) {
    // glTF JSON and the GLB container are one family: cgltf parses both, and
    // import_gltf_file already refuses a .gltf that holds GLB bytes with its own
    // "extension/content mismatch" message. Folding them here keeps that the
    // single owner of the check instead of reporting it twice, differently.
    return format == MeshImportFormat::Glb ? MeshImportFormat::Gltf : format;
}

MeshImportFormat sniff_mesh_format(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 4) {
        u32 magic = 0;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        if (magic == 0x4E464D45u) return MeshImportFormat::NfMesh; // 'NFME', as MeshAsset writes it
        if (std::memcmp(bytes.data(), "glTF", 4) == 0) return MeshImportFormat::Glb;
    }

    usize i = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) i = 3;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (i < bytes.size() && bytes[i] == '{') return MeshImportFormat::Gltf;

    if (bytes.size() >= 3 && std::memcmp(bytes.data(), "ply", 3) == 0 &&
        (bytes.size() == 3 || bytes[3] == '\n' || bytes[3] == '\r')) {
        return MeshImportFormat::Ply;
    }

    // The size test settles every binary STL, including the ones whose 80-byte
    // header starts with the word "solid" — a well-known false positive that
    // content-only sniffing gets wrong.
    if (looks_like_binary_stl(bytes)) return MeshImportFormat::Stl;
    if (starts_with_solid_token(bytes)) return MeshImportFormat::Stl;

    // OBJ has no signature by design of the format; an OBJ file is only
    // identifiable through its extension. Unknown is the honest answer.
    return MeshImportFormat::Unknown;
}

const char* image_extension_for_bytes(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G') {
        return ".png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ".jpg";
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "BM", 2) == 0) {
        return ".bmp";
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "GIF8", 4) == 0) {
        return ".gif";
    }
    // RIFF....WEBP: the container tag is at offset 8, not 0.
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
        return ".webp";
    }
    // TGA deliberately absent: the format has no magic number at all, so a
    // sniffed answer would be a guess. Callers fall back to the source's own
    // extension for it (see the MTL reader below).
    return "";
}

// ---------------------------------------------------------------------------
// .nfmesh — the engine's own container, a pass-through
// ---------------------------------------------------------------------------

namespace {

MeshImportResult import_nfmesh(std::span<const uint8_t> bytes, const std::string& logical_path) {
    MeshImportResult result;
    result.format = MeshImportFormat::NfMesh;
    std::string err;
    std::unique_ptr<MeshAsset> asset = MeshAsset::load_from_bytes(bytes, err);
    if (!asset) {
        result.error = "invalid .nfmesh: " + err;
        return result;
    }
    result.mesh_names.push_back(stem_of(logical_path));
    result.meshes.push_back(std::move(asset));
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Wavefront material libraries (.mtl)
//
// An OBJ carries NO material parameters: `usemtl <name>` names one and the
// values live in the .mtl that the OBJ's `mtllib` line points at. Reading only
// the OBJ therefore throws away every colour the artist authored — which is
// exactly the kind of silent loss this module exists to stop. What the .mtl
// declares and this pipeline cannot store (specular, shininess, bump maps) is
// recorded per material in `dropped`, by name.
// ---------------------------------------------------------------------------

struct MtlEntry {
    GltfMaterialInfo info;
    std::string albedo_path; // raw map_Kd value, resolved against `dir`
    std::string dir;         // directory of the .mtl that defined it
};

void note_dropped(GltfMaterialInfo& info, const std::string& key, const char* reason) {
    // One note per statement kind: a material that declares Ka, Ks, Ns and illum
    // gets four notes, not four hundred.
    for (const std::string& existing : info.dropped) {
        if (existing.rfind(key + " ", 0) == 0) return;
    }
    info.dropped.push_back(key + " " + reason);
}

/// The filename a `map_*` statement points at. Every MTL map option begins with
/// '-' and precedes the name, so the LAST token is the name. MTL has no quoting,
/// so a filename containing spaces is not representable — the same limitation
/// every MTL reader has, stated here rather than half-handled.
std::string mtl_map_filename(const std::vector<std::string_view>& tokens, usize from) {
    if (tokens.size() <= from) return {};
    const std::string_view last = tokens.back();
    if (last.empty() || last[0] == '-') return {};
    return std::string(last);
}

void parse_mtl(const std::string& text, const std::string& mtl_dir, std::vector<MtlEntry>& out) {
    std::vector<std::string_view> tokens;
    LineScanner lines(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    std::string_view raw_line;
    MtlEntry* current = nullptr;

    while (lines.next(raw_line)) {
        const std::string_view line = strip_comment(raw_line);
        split_tokens(line, tokens);
        if (tokens.empty()) continue;
        const std::string_view key = tokens[0];

        if (key == "newmtl") {
            MtlEntry entry;
            entry.dir = mtl_dir;
            entry.info.name = tokens.size() > 1 ? std::string(tokens[1]) : std::string{};
            out.push_back(std::move(entry));
            current = &out.back();
            continue;
        }
        // A statement before any `newmtl` has no material to belong to.
        if (current == nullptr) continue;

        auto read_floats = [&tokens](float* dst, int count) {
            for (int i = 0; i < count; ++i) {
                const usize index = static_cast<usize>(i) + 1;
                if (tokens.size() <= index || !parse_float(tokens[index], dst[i])) return false;
            }
            return true;
        };

        if (key == "Kd") {
            read_floats(current->info.base_color, 3);
        } else if (key == "Ke") {
            // Emissive colour maps onto the engine's emissive factor directly —
            // a faithful mapping, not a substitution.
            read_floats(current->info.emissive, 3);
        } else if (key == "d" || key == "Tr") {
            float value = 1.0f;
            if (tokens.size() > 1 && parse_float(tokens[1], value)) {
                // `d` is opacity; `Tr` is its inverse. Both spellings are in the
                // wild, and reading one as the other would invert every alpha.
                current->info.base_color[3] = (key == "Tr") ? (1.0f - value) : value;
            }
        } else if (key == "map_Kd") {
            current->albedo_path = mtl_map_filename(tokens, 1);
        } else if (key == "Ka" || key == "Ks" || key == "Ns" || key == "Ni" || key == "Tf" ||
                   key == "illum" || key == "sharpness") {
            // Deliberately NOT approximated. Turning a Blinn-Phong exponent (Ns)
            // into a roughness, or a specular colour (Ks) into metallic, would
            // hand the renderer a material the artist never authored.
            note_dropped(current->info, std::string(key),
                         "(no equivalent in this pipeline's parameter set)");
        } else if (key == "map_Ka" || key == "map_Ks" || key == "map_Ns" || key == "map_Bump" ||
                   key == "bump" || key == "disp" || key == "decal" || key == "refl" ||
                   key == "map_d" || key == "norm") {
            note_dropped(current->info, std::string(key), "(only the base-colour map is carried)");
        }
        // Any other statement is ignored by name, never by accident.
    }
}

// ---------------------------------------------------------------------------
// Wavefront OBJ
// ---------------------------------------------------------------------------

/// One object/group being built. OBJ indices are file-global, so the position,
/// UV and normal pools live outside; this holds only what a group owns.
struct ObjGroup {
    std::string name;
    std::vector<AssetVertex> vertices;
    std::vector<u32> indices;
    std::vector<AssetSubMesh> submeshes;
    /// Dedupe key: the (v, vt, vn) triple. OBJ identifies a vertex by its index
    /// triple, not by its float values — so deduping on the triple recovers the
    /// original vertex set exactly, where deduping on values would merge two
    /// vertices the file deliberately kept apart.
    std::map<std::array<long, 3>, u32> vertex_map;
    u32 current_slot = 0;
    u32 run_first_index = 0;
    bool run_open = false;
    bool run_has_faces = false;
};

/// Resolves one OBJ index reference. OBJ indices are 1-based, and a negative
/// index counts backwards from the end of the pool as it stands right now.
bool resolve_obj_index(long raw, usize pool_size, u32& out) {
    long resolved = 0;
    if (raw > 0) {
        resolved = raw - 1;
    } else if (raw < 0) {
        resolved = static_cast<long>(pool_size) + raw;
    } else {
        return false; // index 0 does not exist in OBJ
    }
    if (resolved < 0 || static_cast<usize>(resolved) >= pool_size) return false;
    out = static_cast<u32>(resolved);
    return true;
}

struct ObjRef {
    long v = 0;
    long vt = 0; // 0 == absent
    long vn = 0; // 0 == absent
};

/// "v", "v/vt", "v//vn", "v/vt/vn". An empty field means absent.
bool parse_obj_ref(std::string_view tok, ObjRef& out) {
    out = ObjRef{};
    std::string_view parts[3];
    usize count = 0;
    usize begin = 0;
    for (usize i = 0; i <= tok.size(); ++i) {
        if (i == tok.size() || tok[i] == '/') {
            if (count < 3) parts[count] = tok.substr(begin, i - begin);
            ++count;
            begin = i + 1;
        }
    }
    if (count > 3) return false;

    auto to_long = [](std::string_view s, long& v) {
        if (s.empty()) return false;
        const std::string tmp(s);
        char* end = nullptr;
        const long parsed = std::strtol(tmp.c_str(), &end, 10);
        if (end == tmp.c_str()) return false;
        v = parsed;
        return true;
    };

    if (!to_long(parts[0], out.v)) return false;
    if (count > 1 && !parts[1].empty() && !to_long(parts[1], out.vt)) return false;
    if (count > 2 && !parts[2].empty() && !to_long(parts[2], out.vn)) return false;
    return true;
}

/// Closes the submesh run that is currently accumulating, if it has faces.
void close_obj_run(ObjGroup& group) {
    if (!group.run_open) return;
    const u32 count = static_cast<u32>(group.indices.size()) - group.run_first_index;
    if (group.run_has_faces && count > 0) {
        AssetSubMesh sub;
        sub.index_offset = group.run_first_index;
        sub.index_count = count;
        sub.vertex_offset = 0;
        sub.vertex_count = static_cast<u32>(group.vertices.size());
        sub.material_slot = group.current_slot;
        group.submeshes.push_back(sub);
    }
    group.run_first_index = static_cast<u32>(group.indices.size());
    group.run_has_faces = false;
}

MeshImportResult import_obj(std::span<const uint8_t> bytes, const std::string& logical_path) {
    MeshImportResult result;
    result.format = MeshImportFormat::Obj;

    std::vector<float> positions; // 3 per v
    std::vector<float> uvs;       // 2 per vt
    std::vector<float> normals;   // 3 per vn
    std::vector<ObjGroup> groups;
    std::map<std::string, u32> slot_of_name;
    bool saw_any_normal = false;

    // --- material libraries -------------------------------------------------
    // `mtllib` is scanned in its own pass rather than inline, so a file that
    // puts `usemtl` before its `mtllib` still gets its material parameters.
    // Convention says mtllib comes first; robustness says do not depend on it.
    const std::filesystem::path obj_dir = std::filesystem::path(logical_path).parent_path();
    std::map<std::string, MtlEntry> mtl_by_name;
    {
        std::vector<std::string_view> scan_tokens;
        LineScanner scan(bytes);
        std::string_view scan_line;
        while (scan.next(scan_line)) {
            split_tokens(strip_comment(scan_line), scan_tokens);
            if (scan_tokens.size() < 2 || scan_tokens[0] != "mtllib") continue;
            const std::string mtl_name(scan_tokens[1]);
            const std::string mtl_path = (obj_dir / mtl_name).string();
            std::vector<uint8_t> mtl_bytes;
            std::string read_error;
            if (!read_all_bytes(mtl_path, mtl_bytes, read_error)) {
                result.warnings.push_back("OBJ references '" + mtl_name +
                                          "' but it could not be read; its materials keep defaults");
                continue;
            }
            const std::string mtl_text(mtl_bytes.begin(), mtl_bytes.end());
            std::vector<MtlEntry> entries;
            parse_mtl(mtl_text, (obj_dir / mtl_name).parent_path().string(), entries);
            for (MtlEntry& entry : entries) {
                mtl_by_name[entry.info.name] = std::move(entry);
            }
        }
    }

    // Turns one `usemtl` name into a material list entry, loading its base-colour
    // map on the way. Returns the slot — which IS the index into
    // result.materials, so a submesh's material_slot points at a real material
    // instead of an arbitrary number.
    auto material_slot_for = [&](const std::string& name) -> u32 {
        auto known = slot_of_name.find(name);
        if (known != slot_of_name.end()) return known->second;

        const u32 slot = static_cast<u32>(result.materials.size());
        slot_of_name[name] = slot;

        GltfMaterialInfo info;
        info.name = name;
        auto entry = mtl_by_name.find(name);
        if (entry != mtl_by_name.end()) {
            const std::string albedo_path = entry->second.albedo_path;
            const std::string mtl_dir = entry->second.dir;
            info = entry->second.info;
            info.name = name;
            if (!albedo_path.empty()) {
                std::vector<uint8_t> image_bytes;
                std::string image_error;
                const std::string image_path = (std::filesystem::path(mtl_dir) / albedo_path).string();
                if (read_all_bytes(image_path, image_bytes, image_error)) {
                    MeshImportImage image;
                    image.name = std::filesystem::path(albedo_path).stem().string();
                    if (image.name.empty()) image.name = name;
                    image.source = "mtllib map_Kd " + albedo_path;
                    const char* sniffed = image_extension_for_bytes(image_bytes);
                    // TGA has no magic number, so the source's own extension is
                    // the only evidence there is; it is a fallback, not a guess
                    // dressed up as one.
                    std::string fallback = std::filesystem::path(albedo_path).extension().string();
                    std::transform(fallback.begin(), fallback.end(), fallback.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    image.extension = sniffed[0] != '\0' ? std::string(sniffed) : fallback;
                    image.bytes = std::move(image_bytes);
                    info.albedo_image = static_cast<int>(result.images.size());
                    result.images.push_back(std::move(image));
                } else {
                    ++result.images_skipped;
                    result.warnings.push_back("material '" + name + "' maps its base colour to '" +
                                              albedo_path + "', which could not be read");
                }
            }
        }
        result.materials.push_back(std::move(info));
        return slot;
    };

    auto current_group = [&]() -> ObjGroup& {
        if (groups.empty()) {
            groups.emplace_back();
            groups.back().name = stem_of(logical_path);
        }
        return groups.back();
    };
    // A new object/group name starts a new MeshAsset, and therefore closes the
    // current one. `o` and `g` are treated alike: both are real boundaries.
    auto begin_group = [&](std::string_view name) {
        if (!groups.empty()) {
            close_obj_run(groups.back());
            // An empty group (a name with no faces under it) is dropped rather
            // than emitted as a zero-triangle mesh.
            if (groups.back().submeshes.empty()) groups.pop_back();
        }
        ObjGroup g;
        g.name = name.empty() ? stem_of(logical_path) : std::string(name);
        groups.push_back(std::move(g));
    };

    std::vector<std::string_view> tokens;
    LineScanner lines(bytes);
    std::string_view raw_line;
    while (lines.next(raw_line)) {
        const std::string_view line = strip_comment(raw_line);
        split_tokens(line, tokens);
        if (tokens.empty()) continue;
        const std::string_view key = tokens[0];

        if (key == "v" || key == "V") {
            if (tokens.size() < 4) {
                ++result.vertices_skipped;
                continue;
            }
            float v[3] = {0.0f, 0.0f, 0.0f};
            if (!parse_float(tokens[1], v[0]) || !parse_float(tokens[2], v[1]) ||
                !parse_float(tokens[3], v[2]) || !finite_vec3(v)) {
                ++result.vertices_skipped;
                continue;
            }
            positions.push_back(v[0]);
            positions.push_back(v[1]);
            positions.push_back(v[2]);
        } else if (key == "vt") {
            float t[2] = {0.0f, 0.0f};
            const bool ok0 = tokens.size() > 1 && parse_float(tokens[1], t[0]);
            const bool ok1 = tokens.size() > 2 && parse_float(tokens[2], t[1]);
            if (!ok0) {
                ++result.vertices_skipped;
                continue;
            }
            uvs.push_back(t[0]);
            uvs.push_back(ok1 ? t[1] : 0.0f);
        } else if (key == "vn") {
            if (tokens.size() < 4) {
                ++result.vertices_skipped;
                continue;
            }
            float n[3] = {0.0f, 0.0f, 0.0f};
            if (!parse_float(tokens[1], n[0]) || !parse_float(tokens[2], n[1]) ||
                !parse_float(tokens[3], n[2]) || !finite_vec3(n)) {
                ++result.vertices_skipped;
                continue;
            }
            normals.push_back(n[0]);
            normals.push_back(n[1]);
            normals.push_back(n[2]);
            saw_any_normal = true;
        } else if (key == "o" || key == "g") {
            begin_group(tokens.size() > 1 ? tokens[1] : std::string_view{});
        } else if (key == "usemtl") {
            ObjGroup& group = current_group();
            close_obj_run(group);
            const std::string name(tokens.size() > 1 ? tokens[1] : std::string_view{});
            // The slot is the material's index in first-use order. That makes
            // material_slot mean something to a consumer, and it still preserves
            // MeshExport's own naming: it writes `usemtl slot0`, `slot1`, ... in
            // submesh order, so first-use order reproduces the original numbers.
            group.current_slot = material_slot_for(name);
        } else if (key == "f") {
            ObjGroup& group = current_group();
            if (!group.run_open) {
                group.run_open = true;
                group.run_first_index = static_cast<u32>(group.indices.size());
                group.run_has_faces = false;
            }

            const usize ref_count = tokens.size() - 1;
            if (ref_count < 3) {
                ++result.faces_skipped;
                continue;
            }
            std::vector<u32> face;
            face.reserve(ref_count);
            bool face_ok = true;
            // Vertices this face adds are rolled back if a later reference in
            // the SAME face turns out to be unusable. Without this, a face that
            // fails on its third vertex leaves the first two behind as orphans:
            // never drawn, but counted in the asset's vertex array and in every
            // submesh's vertex_count. GltfImport rolls a primitive back the same
            // way for the same reason.
            const usize vertex_base = group.vertices.size();
            for (usize i = 1; i < tokens.size() && face_ok; ++i) {
                ObjRef ref;
                if (!parse_obj_ref(tokens[i], ref)) {
                    face_ok = false;
                    break;
                }
                u32 v_index = 0, vt_index = 0, vn_index = 0;
                if (!resolve_obj_index(ref.v, positions.size() / 3, v_index)) {
                    face_ok = false;
                    break;
                }
                if (ref.vt != 0 && !resolve_obj_index(ref.vt, uvs.size() / 2, vt_index)) {
                    face_ok = false;
                    break;
                }
                if (ref.vn != 0 && !resolve_obj_index(ref.vn, normals.size() / 3, vn_index)) {
                    face_ok = false;
                    break;
                }

                // The key is the RESOLVED index triple, not the raw reference.
                // A file may write the same vertex as `-4/-4/1` in one face and
                // `1/1/1` in the next; keying on the raw text would call those
                // two different vertices and split the mesh apart.
                const std::array<long, 3> dedupe_key = {
                    static_cast<long>(v_index), ref.vt != 0 ? static_cast<long>(vt_index) : -1,
                    ref.vn != 0 ? static_cast<long>(vn_index) : -1};
                auto found = group.vertex_map.find(dedupe_key);
                if (found != group.vertex_map.end()) {
                    face.push_back(found->second);
                    continue;
                }

                AssetVertex vertex;
                const float* p = positions.data() + static_cast<usize>(v_index) * 3;
                vertex.position[0] = p[0];
                vertex.position[1] = p[1];
                vertex.position[2] = p[2];
                if (ref.vt != 0) {
                    const float* t = uvs.data() + static_cast<usize>(vt_index) * 2;
                    vertex.uv0[0] = t[0];
                    vertex.uv0[1] = t[1];
                }
                if (ref.vn != 0) {
                    const float* n = normals.data() + static_cast<usize>(vn_index) * 3;
                    vertex.normal[0] = n[0];
                    vertex.normal[1] = n[1];
                    vertex.normal[2] = n[2];
                }
                const u32 new_index = static_cast<u32>(group.vertices.size());
                group.vertices.push_back(vertex);
                group.vertex_map.emplace(dedupe_key, new_index);
                face.push_back(new_index);
            }
            if (!face_ok) {
                ++result.faces_skipped;
                if (group.vertices.size() > vertex_base) {
                    group.vertices.resize(vertex_base);
                    auto it = group.vertex_map.begin();
                    while (it != group.vertex_map.end()) {
                        if (it->second >= vertex_base) {
                            it = group.vertex_map.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                continue;
            }

            // n-gon -> fan. Every triangle shares vertex 0, which is the
            // standard decomposition and keeps the winding the file declared.
            const usize first_index = group.indices.size();
            for (usize i = 1; i + 1 < face.size(); ++i) {
                group.indices.push_back(face[0]);
                group.indices.push_back(face[i]);
                group.indices.push_back(face[i + 1]);
            }
            if (group.indices.size() == first_index) {
                ++result.faces_skipped;
                continue;
            }
            group.run_has_faces = true;
        }
        // mtllib / s / l / p and every other OBJ statement: not geometry this
        // engine consumes. Ignored by name, never by accident.
    }

    if (!groups.empty()) close_obj_run(groups.back());

    for (ObjGroup& group : groups) {
        if (group.submeshes.empty() || group.indices.empty()) continue;
        auto mesh = std::make_unique<MeshAsset>();
        mesh->logical_path = logical_path + "#" + group.name;
        mesh->vertices = std::move(group.vertices);
        mesh->indices = std::move(group.indices);
        mesh->submeshes = std::move(group.submeshes);
        // OBJ interleaves its vertices, so a submesh's indices do not sit in one
        // contiguous vertex range. Each submesh therefore spans the whole vertex
        // array (offset 0, all vertices) — the same shape MeshExport's
        // writable_submeshes() uses for a hand-built asset. A narrower range
        // would be a claim the file cannot support.
        for (AssetSubMesh& sub : mesh->submeshes) {
            sub.vertex_offset = 0;
            sub.vertex_count = static_cast<u32>(mesh->vertices.size());
        }
        if (!saw_any_normal) {
            compute_smooth_normals(mesh->vertices, mesh->indices, 0, 0, mesh->indices.size());
        }
        recompute_mesh_bounds(*mesh);
        result.mesh_names.push_back(group.name);
        result.meshes.push_back(std::move(mesh));
    }

    if (result.meshes.empty()) {
        result.error = "OBJ contains no usable faces";
        return result;
    }
    if (!saw_any_normal) {
        result.warnings.push_back(
            "OBJ has no vertex normals (vn); smooth normals were computed");
    }
    // uv1 and tangents are simply not in the format — stated once, in the
    // header's format table, rather than repeated as a warning per import.
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// STL — triangle soup, binary or ASCII
// ---------------------------------------------------------------------------

struct StlTriangle {
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float position[3][3] = {{0.0f}};
};

/// The normal a triangle gets when the file's own normal is unusable. STL's
/// facet normal is what the exporter computed from the winding, so recomputing
/// it from the winding reproduces exactly that convention.
void stl_normal_from_winding(const float p[3][3], float out[3]) {
    const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
    const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-20f) {
        nx /= len;
        ny /= len;
        nz /= len;
    } else {
        nx = 0.0f;
        ny = 0.0f;
        nz = 1.0f;
    }
    out[0] = nx;
    out[1] = ny;
    out[2] = nz;
}

bool read_stl_binary(std::span<const uint8_t> bytes, std::vector<StlTriangle>& out,
                     std::string& error) {
    u32 count = 0;
    std::memcpy(&count, bytes.data() + 80, sizeof(count));
    if (static_cast<usize>(count) > kAbsurdCount) {
        error = "STL triangle count is absurd (corrupt header)";
        return false;
    }
    if (84u + static_cast<usize>(count) * 50u != bytes.size()) {
        error = "binary STL size does not match its triangle count";
        return false;
    }
    out.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const uint8_t* p = bytes.data() + 84 + i * 50;
        StlTriangle tri;
        std::memcpy(tri.normal, p, 12);
        std::memcpy(tri.position, p + 12, 36);
        // The 2-byte attribute count that follows is deliberately not read:
        // every STL producer means something different by it.
        if (!finite_vec3(tri.normal)) tri.normal[0] = tri.normal[1] = tri.normal[2] = 0.0f;
        out.push_back(tri);
    }
    return true;
}

bool read_stl_ascii(std::span<const uint8_t> bytes, std::vector<StlTriangle>& out) {
    TokenScanner scanner(bytes);
    float pending_normal[3] = {0.0f, 0.0f, 0.0f};
    bool have_normal = false;
    float corners[3][3] = {{0.0f}};
    int corner = 0;

    while (!scanner.at_end()) {
        const std::string_view tok = scanner.next();
        if (tok == "facet") {
            const std::string_view maybe_normal = scanner.next();
            if (maybe_normal == "normal") {
                have_normal = scanner.next_float(pending_normal[0]) &&
                              scanner.next_float(pending_normal[1]) &&
                              scanner.next_float(pending_normal[2]);
            }
        } else if (tok == "vertex") {
            float v[3] = {0.0f, 0.0f, 0.0f};
            if (!scanner.next_float(v[0]) || !scanner.next_float(v[1]) ||
                !scanner.next_float(v[2])) {
                continue;
            }
            if (corner < 3) {
                corners[corner][0] = v[0];
                corners[corner][1] = v[1];
                corners[corner][2] = v[2];
            }
            ++corner;
            if (corner == 3) {
                StlTriangle tri;
                for (int c = 0; c < 3; ++c) {
                    tri.position[c][0] = corners[c][0];
                    tri.position[c][1] = corners[c][1];
                    tri.position[c][2] = corners[c][2];
                }
                if (have_normal && finite_vec3(pending_normal)) {
                    tri.normal[0] = pending_normal[0];
                    tri.normal[1] = pending_normal[1];
                    tri.normal[2] = pending_normal[2];
                }
                out.push_back(tri);
                corner = 0;
                have_normal = false;
            }
        }
    }
    return true;
}

MeshImportResult import_stl(std::span<const uint8_t> bytes, const std::string& logical_path) {
    MeshImportResult result;
    result.format = MeshImportFormat::Stl;

    std::vector<StlTriangle> triangles;
    if (looks_like_binary_stl(bytes)) {
        std::string error;
        if (!read_stl_binary(bytes, triangles, error)) {
            result.error = "invalid binary STL: " + error;
            return result;
        }
    } else if (starts_with_solid_token(bytes)) {
        read_stl_ascii(bytes, triangles);
    } else {
        // Neither shape. The size test is authoritative for binary, so reaching
        // here means the file is truncated or is not an STL at all.
        result.error =
            "not an STL: neither a binary STL whose size matches its triangle count, nor an "
            "ASCII STL starting with 'solid'";
        return result;
    }

    if (triangles.empty()) {
        result.error = "STL contains no triangles";
        return result;
    }

    auto mesh = std::make_unique<MeshAsset>();
    mesh->logical_path = logical_path;
    mesh->vertices.reserve(triangles.size() * 3);
    mesh->indices.reserve(triangles.size() * 3);

    // STL is a triangle soup: no index buffer, no UVs, one face normal per
    // triangle. Vertices are re-shared only where both the position AND the
    // normal agree bit for bit, which is what makes a flat-shaded cube come
    // back as 24 vertices instead of 36 — without ever inventing a smooth
    // normal the file did not have.
    std::map<std::array<u32, 6>, u32> vertex_map;
    auto key_component = [](float value) {
        // -0.0f and 0.0f are the same position; folding them here keeps the
        // dedupe from splitting a vertex over a signed zero.
        const float normalized = (value == 0.0f) ? 0.0f : value;
        u32 bits = 0;
        std::memcpy(&bits, &normalized, sizeof(bits));
        return bits;
    };

    for (const StlTriangle& tri : triangles) {
        float normal[3] = {tri.normal[0], tri.normal[1], tri.normal[2]};
        const float len =
            std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (!(len > 0.5f && len < 2.0f)) {
            // Not a plausible unit normal (0,0,0 is the common placeholder), so
            // the winding decides instead of the file's unusable field.
            stl_normal_from_winding(tri.position, normal);
        }
        for (int c = 0; c < 3; ++c) {
            if (!finite_vec3(tri.position[c])) {
                ++result.vertices_skipped;
                continue;
            }
            const std::array<u32, 6> key = {key_component(tri.position[c][0]),
                                            key_component(tri.position[c][1]),
                                            key_component(tri.position[c][2]),
                                            key_component(normal[0]),
                                            key_component(normal[1]),
                                            key_component(normal[2])};
            auto found = vertex_map.find(key);
            if (found != vertex_map.end()) {
                mesh->indices.push_back(found->second);
                continue;
            }
            AssetVertex vertex;
            vertex.position[0] = tri.position[c][0];
            vertex.position[1] = tri.position[c][1];
            vertex.position[2] = tri.position[c][2];
            vertex.normal[0] = normal[0];
            vertex.normal[1] = normal[1];
            vertex.normal[2] = normal[2];
            const u32 index = static_cast<u32>(mesh->vertices.size());
            mesh->vertices.push_back(vertex);
            vertex_map.emplace(key, index);
            mesh->indices.push_back(index);
        }
    }

    // A triangle whose corners did not all survive contributes nothing; drop
    // its partial tail so the index buffer stays a multiple of three.
    if (mesh->indices.size() % 3 != 0) {
        const usize dropped = mesh->indices.size() % 3;
        mesh->indices.resize(mesh->indices.size() - dropped);
        ++result.faces_skipped;
    }
    if (mesh->indices.empty()) {
        result.error = "STL contains no usable triangles (all vertices were non-finite)";
        return result;
    }

    AssetSubMesh sub;
    sub.index_offset = 0;
    sub.index_count = static_cast<u32>(mesh->indices.size());
    sub.vertex_offset = 0;
    sub.vertex_count = static_cast<u32>(mesh->vertices.size());
    sub.material_slot = 0;
    mesh->submeshes.push_back(sub);
    recompute_mesh_bounds(*mesh);

    result.warnings.push_back(
        "STL has no index buffer, no UVs and no vertex normals; geometry is flat-shaded and "
        "UVs default to zero");
    result.mesh_names.push_back(stem_of(logical_path));
    result.meshes.push_back(std::move(mesh));
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Stanford PLY — ASCII and binary, both endiannesses
// ---------------------------------------------------------------------------

enum class PlyType : uint8_t {
    Unknown = 0,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Float32,
    Float64,
};

usize ply_type_size(PlyType type) {
    switch (type) {
        case PlyType::Int8:
        case PlyType::UInt8: return 1;
        case PlyType::Int16:
        case PlyType::UInt16: return 2;
        case PlyType::Int32:
        case PlyType::UInt32:
        case PlyType::Float32: return 4;
        case PlyType::Float64: return 8;
        default: return 0;
    }
}

PlyType ply_type_from_name(std::string_view name) {
    if (name == "char" || name == "int8") return PlyType::Int8;
    if (name == "uchar" || name == "uint8") return PlyType::UInt8;
    if (name == "short" || name == "int16") return PlyType::Int16;
    if (name == "ushort" || name == "uint16") return PlyType::UInt16;
    if (name == "int" || name == "int32") return PlyType::Int32;
    if (name == "uint" || name == "uint32") return PlyType::UInt32;
    if (name == "float" || name == "float32") return PlyType::Float32;
    if (name == "double" || name == "float64") return PlyType::Float64;
    return PlyType::Unknown;
}

struct PlyProperty {
    std::string name;
    PlyType type = PlyType::Unknown;
    bool is_list = false;
    PlyType count_type = PlyType::Unknown;
};

struct PlyElement {
    std::string name;
    usize count = 0;
    std::vector<PlyProperty> properties;
};

enum class PlyEncoding : uint8_t { Ascii, BinaryLittle, BinaryBig };

struct PlyReader {
    std::span<const uint8_t> bytes;
    PlyEncoding encoding = PlyEncoding::Ascii;
    usize cursor = 0;
    TokenScanner* tokens = nullptr;

    bool binary() const { return encoding != PlyEncoding::Ascii; }

    bool read_value(PlyType type, double& out) {
        if (type == PlyType::Unknown) return false;
        if (!binary()) {
            if (tokens == nullptr) return false;
            return tokens->next_double(out);
        }
        const usize size = ply_type_size(type);
        if (size == 0 || cursor + size > bytes.size()) return false;
        const uint8_t* p = bytes.data() + cursor;
        cursor += size;
        const bool swap = encoding == PlyEncoding::BinaryBig;
        switch (type) {
            case PlyType::Int8: {
                int8_t v = 0;
                std::memcpy(&v, p, 1);
                out = static_cast<double>(v);
                return true;
            }
            case PlyType::UInt8: {
                uint8_t v = 0;
                std::memcpy(&v, p, 1);
                out = static_cast<double>(v);
                return true;
            }
            case PlyType::Int16: {
                u16 raw = 0;
                std::memcpy(&raw, p, 2);
                if (swap) raw = bswap16(raw);
                int16_t v = 0;
                std::memcpy(&v, &raw, 2);
                out = static_cast<double>(v);
                return true;
            }
            case PlyType::UInt16: {
                u16 raw = 0;
                std::memcpy(&raw, p, 2);
                if (swap) raw = bswap16(raw);
                out = static_cast<double>(raw);
                return true;
            }
            case PlyType::Int32: {
                u32 raw = 0;
                std::memcpy(&raw, p, 4);
                if (swap) raw = bswap32(raw);
                int32_t v = 0;
                std::memcpy(&v, &raw, 4);
                out = static_cast<double>(v);
                return true;
            }
            case PlyType::UInt32: {
                u32 raw = 0;
                std::memcpy(&raw, p, 4);
                if (swap) raw = bswap32(raw);
                out = static_cast<double>(raw);
                return true;
            }
            case PlyType::Float32: {
                u32 raw = 0;
                std::memcpy(&raw, p, 4);
                if (swap) raw = bswap32(raw);
                float v = 0.0f;
                std::memcpy(&v, &raw, 4);
                out = static_cast<double>(v);
                return true;
            }
            case PlyType::Float64: {
                u64 raw = 0;
                std::memcpy(&raw, p, 8);
                if (swap) raw = bswap64(raw);
                double v = 0.0;
                std::memcpy(&v, &raw, 8);
                out = v;
                return true;
            }
            default: return false;
        }
    }

    /// Reads a list property's count and then every value, or skips both when
    /// the caller does not want them. Unknown elements have to be consumed
    /// exactly, or every element after them lands on the wrong byte.
    bool read_list(const PlyProperty& prop, std::vector<long>* out_values) {
        double raw_count = 0.0;
        if (!read_value(prop.count_type, raw_count)) return false;
        if (!std::isfinite(raw_count) || raw_count < 0.0 || raw_count > 1000000.0) return false;
        const long count = static_cast<long>(raw_count);
        if (out_values != nullptr) out_values->clear();
        for (long i = 0; i < count; ++i) {
            double value = 0.0;
            if (!read_value(prop.type, value)) return false;
            if (out_values != nullptr) out_values->push_back(static_cast<long>(value));
        }
        return true;
    }
};

MeshImportResult import_ply(std::span<const uint8_t> bytes, const std::string& logical_path) {
    MeshImportResult result;
    result.format = MeshImportFormat::Ply;

    LineScanner lines(bytes);
    std::string_view line;
    if (!lines.next(line) || line.substr(0, 3) != "ply") {
        result.error = "not a PLY: missing 'ply' magic line";
        return result;
    }

    PlyEncoding encoding = PlyEncoding::Ascii;
    bool have_encoding = false;
    std::vector<PlyElement> elements;
    std::vector<std::string_view> tokens;

    bool header_done = false;
    while (lines.next(line)) {
        split_tokens(line, tokens);
        if (tokens.empty()) continue;
        if (tokens[0] == "format") {
            if (tokens.size() < 2) {
                result.error = "PLY header: 'format' without an encoding";
                return result;
            }
            if (tokens[1] == "ascii") {
                encoding = PlyEncoding::Ascii;
            } else if (tokens[1] == "binary_little_endian") {
                encoding = PlyEncoding::BinaryLittle;
            } else if (tokens[1] == "binary_big_endian") {
                encoding = PlyEncoding::BinaryBig;
            } else {
                result.error = "PLY header: unsupported encoding '" + std::string(tokens[1]) + "'";
                return result;
            }
            have_encoding = true;
        } else if (tokens[0] == "element") {
            if (tokens.size() < 3) {
                result.error = "PLY header: malformed 'element' line";
                return result;
            }
            PlyElement element;
            element.name = std::string(tokens[1]);
            const std::string count_text(tokens[2]);
            char* end = nullptr;
            const long long count = std::strtoll(count_text.c_str(), &end, 10);
            if (end == count_text.c_str() || count < 0) {
                result.error = "PLY header: element '" + element.name + "' has a bad count";
                return result;
            }
            element.count = static_cast<usize>(count);
            elements.push_back(std::move(element));
        } else if (tokens[0] == "property") {
            if (elements.empty()) {
                result.error = "PLY header: 'property' before any 'element'";
                return result;
            }
            PlyProperty prop;
            if (tokens.size() >= 5 && tokens[1] == "list") {
                prop.is_list = true;
                prop.count_type = ply_type_from_name(tokens[2]);
                prop.type = ply_type_from_name(tokens[3]);
                prop.name = std::string(tokens[4]);
            } else if (tokens.size() >= 3) {
                prop.type = ply_type_from_name(tokens[1]);
                prop.name = std::string(tokens[2]);
            } else {
                result.error = "PLY header: malformed 'property' line";
                return result;
            }
            if (prop.type == PlyType::Unknown ||
                (prop.is_list && prop.count_type == PlyType::Unknown)) {
                result.error = "PLY header: property '" + prop.name + "' has an unknown type";
                return result;
            }
            elements.back().properties.push_back(std::move(prop));
        } else if (tokens[0] == "end_header") {
            header_done = true;
            break;
        }
        // comment / obj_info / anything else: skipped by name.
    }

    if (!header_done) {
        result.error = "PLY header: no 'end_header'";
        return result;
    }
    if (!have_encoding) {
        result.error = "PLY header: no 'format' line";
        return result;
    }

    TokenScanner token_scanner(bytes, lines.byte_offset_after_last_line());
    PlyReader reader;
    reader.bytes = bytes;
    reader.encoding = encoding;
    reader.cursor = lines.byte_offset_after_last_line();
    reader.tokens = &token_scanner;

    std::vector<AssetVertex> vertices;
    std::vector<u32> indices;
    bool have_vertex_element = false;
    bool have_normals = false;

    for (const PlyElement& element : elements) {
        const bool is_vertex = element.name == "vertex";
        const bool is_face = element.name == "face";
        if (is_vertex) have_vertex_element = true;

        // Property name -> index, so a vertex row is read in the file's own
        // order instead of an assumed one.
        auto index_of = [&element](std::string_view name) -> int {
            for (usize i = 0; i < element.properties.size(); ++i) {
                if (element.properties[i].name == name) return static_cast<int>(i);
            }
            return -1;
        };
        const int px = is_vertex ? index_of("x") : -1;
        const int py = is_vertex ? index_of("y") : -1;
        const int pz = is_vertex ? index_of("z") : -1;
        const int pnx = is_vertex ? index_of("nx") : -1;
        const int pny = is_vertex ? index_of("ny") : -1;
        const int pnz = is_vertex ? index_of("nz") : -1;
        const int pu = is_vertex ? index_of("s") : -1;
        const int pv = is_vertex ? index_of("t") : -1;
        const int ptu = (is_vertex && pu < 0) ? index_of("texture_u") : -1;
        const int ptv = (is_vertex && pv < 0) ? index_of("texture_v") : -1;
        const int face_list = is_face ? index_of("vertex_indices") : -1;
        const int face_list_alt = (is_face && face_list < 0) ? index_of("vertex_index") : -1;
        const int target_list = face_list >= 0 ? face_list : face_list_alt;

        if (is_vertex && (px < 0 || py < 0 || pz < 0)) {
            result.error = "PLY vertex element has no x/y/z properties";
            return result;
        }
        if (is_face && target_list < 0) {
            result.warnings.push_back(
                "PLY face element has no vertex_indices property; its faces were skipped");
        }

        std::vector<long> list_values;
        for (usize row = 0; row < element.count; ++row) {
            AssetVertex vertex;
            float values[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            const int slots[8] = {px, py, pz, pnx, pny, pnz, pu, pv};
            bool row_ok = true;
            for (usize pi = 0; pi < element.properties.size() && row_ok; ++pi) {
                const PlyProperty& prop = element.properties[pi];
                if (prop.is_list) {
                    const bool wanted = is_face && static_cast<int>(pi) == target_list;
                    row_ok = reader.read_list(prop, wanted ? &list_values : nullptr);
                    continue;
                }
                double value = 0.0;
                if (!reader.read_value(prop.type, value)) {
                    row_ok = false;
                    break;
                }
                if (!is_vertex) continue;
                for (int s = 0; s < 8; ++s) {
                    if (slots[s] == static_cast<int>(pi)) {
                        values[s] = static_cast<float>(value);
                        break;
                    }
                }
                if (ptu == static_cast<int>(pi)) values[6] = static_cast<float>(value);
                if (ptv == static_cast<int>(pi)) values[7] = static_cast<float>(value);
            }
            if (!row_ok) {
                result.error = "PLY data ended early (element '" + element.name + "', row " +
                               std::to_string(row) + ")";
                return result;
            }

            if (is_vertex) {
                if (!finite_vec3(values)) {
                    ++result.vertices_skipped;
                    // Keep the vertex so face indices stay valid; a non-finite
                    // position is still a slot in the index space.
                }
                vertex.position[0] = values[0];
                vertex.position[1] = values[1];
                vertex.position[2] = values[2];
                vertex.normal[0] = values[3];
                vertex.normal[1] = values[4];
                vertex.normal[2] = values[5];
                vertex.uv0[0] = values[6];
                vertex.uv0[1] = values[7];
                if (pnx >= 0) have_normals = true;
                vertices.push_back(vertex);
            } else if (is_face) {
                if (target_list < 0 || list_values.size() < 3) {
                    ++result.faces_skipped;
                    continue;
                }
                std::vector<u32> face;
                face.reserve(list_values.size());
                bool face_ok = true;
                for (long raw : list_values) {
                    if (raw < 0 || static_cast<usize>(raw) >= vertices.size()) {
                        face_ok = false;
                        break;
                    }
                    face.push_back(static_cast<u32>(raw));
                }
                if (!face_ok) {
                    ++result.faces_skipped;
                    continue;
                }
                for (usize i = 1; i + 1 < face.size(); ++i) {
                    indices.push_back(face[0]);
                    indices.push_back(face[i]);
                    indices.push_back(face[i + 1]);
                }
            }
            // Any other element (edges, materials, per-face colours): its bytes
            // were consumed by the loop above, and its content is not geometry.
        }
    }

    if (!have_vertex_element) {
        result.error = "PLY has no vertex element";
        return result;
    }
    if (indices.empty()) {
        result.error = "PLY has no usable faces (a point cloud is not a triangle mesh)";
        return result;
    }

    auto mesh = std::make_unique<MeshAsset>();
    mesh->logical_path = logical_path;
    mesh->vertices = std::move(vertices);
    mesh->indices = std::move(indices);
    if (!have_normals) {
        compute_smooth_normals(mesh->vertices, mesh->indices, 0, 0, mesh->indices.size());
    }
    AssetSubMesh sub;
    sub.index_offset = 0;
    sub.index_count = static_cast<u32>(mesh->indices.size());
    sub.vertex_offset = 0;
    sub.vertex_count = static_cast<u32>(mesh->vertices.size());
    sub.material_slot = 0;
    mesh->submeshes.push_back(sub);
    recompute_mesh_bounds(*mesh);

    if (!have_normals) {
        result.warnings.push_back(
            "PLY has no vertex normals (nx/ny/nz); smooth normals were computed");
    }
    result.mesh_names.push_back(stem_of(logical_path));
    result.meshes.push_back(std::move(mesh));
    result.ok = true;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

MeshImportResult import_mesh_as(std::span<const uint8_t> bytes, MeshImportFormat format,
                                const std::string& logical_path) {
    MeshImportResult result;
    if (bytes.empty()) {
        result.error = "import: empty input";
        return result;
    }
    if (bytes.size() > kAbsurdCount) {
        result.error = "import: input is absurdly large (" + std::to_string(bytes.size()) +
                       " bytes)";
        return result;
    }
    switch (format) {
        case MeshImportFormat::NfMesh:
            return import_nfmesh(bytes, logical_path);
        case MeshImportFormat::Gltf:
        case MeshImportFormat::Glb:
            // The glTF reader owns .gltf/.glb completely — including resolving a
            // sibling .bin, which only it can do from a file path.
            return import_gltf_memory(bytes.data(), bytes.size(), logical_path);
        case MeshImportFormat::Obj:
            return import_obj(bytes, logical_path);
        case MeshImportFormat::Stl:
            return import_stl(bytes, logical_path);
        case MeshImportFormat::Ply:
            return import_ply(bytes, logical_path);
        default:
            result.error = "import: unknown mesh format";
            return result;
    }
}

MeshImportResult import_mesh_memory(std::span<const uint8_t> bytes,
                                    const std::string& logical_path) {
    MeshImportResult result;
    if (bytes.empty()) {
        result.error = "import: empty input";
        return result;
    }

    MeshImportFormat from_extension = MeshImportFormat::Unknown;
    const std::string ext = lowercase_extension(logical_path);
    if (!ext.empty()) {
        mesh_import_format_from_extension(ext, from_extension);
    }
    const MeshImportFormat sniffed = sniff_mesh_format(bytes);

    if (from_extension == MeshImportFormat::Unknown) {
        if (sniffed == MeshImportFormat::Unknown) {
            result.error = "cannot determine the mesh format of '" + logical_path +
                           "': unknown extension and no recognisable signature";
            return result;
        }
        return import_mesh_as(bytes, sniffed, logical_path);
    }

    // A known extension that the bytes plainly contradict is a mistake, not a
    // preference: fail loudly instead of importing something the caller did not
    // ask for. glTF/GLB share a family, so the glTF reader keeps owning its own
    // (stricter) version of this check.
    if (sniffed != MeshImportFormat::Unknown &&
        mesh_import_format_family(sniffed) != mesh_import_format_family(from_extension)) {
        result.format = from_extension;
        result.error = "extension/content mismatch for '" + logical_path + "': the extension "
                       "names " + mesh_import_format_name(from_extension) + " but the bytes are " +
                       mesh_import_format_name(sniffed);
        return result;
    }

    return import_mesh_as(bytes, from_extension, logical_path);
}

MeshImportResult import_mesh_file(const std::string& path) {
    MeshImportResult result;
    if (path.empty()) {
        result.error = "import_mesh_file: empty path";
        return result;
    }

    MeshImportFormat from_extension = MeshImportFormat::Unknown;
    const std::string ext = lowercase_extension(path);
    if (!ext.empty()) {
        mesh_import_format_from_extension(ext, from_extension);
    }

    // glTF is the one family that cannot be served from memory alone: a .gltf
    // may point at a sibling .bin, which only the file-based reader resolves.
    if (from_extension == MeshImportFormat::Gltf || from_extension == MeshImportFormat::Glb) {
        return import_gltf_file(path);
    }

    std::vector<uint8_t> bytes;
    std::string err;
    if (!read_all_bytes(path, bytes, err)) {
        result.error = err;
        return result;
    }
    return import_mesh_memory(bytes, path);
}

} // namespace nf::assets
