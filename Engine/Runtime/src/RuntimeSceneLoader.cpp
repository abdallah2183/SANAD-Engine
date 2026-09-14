#define _CRT_SECURE_NO_WARNINGS
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Core/Logger.hpp>
#include <fstream>
#include <set>
#include <sstream>

namespace nf::runtime {

static std::string trim(const std::string& s) {
    size_t a=0; while(a<s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b=s.size(); while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}

SceneLoadResult load_scene_from_physical(const std::filesystem::path& physical_path) {
    SceneLoadResult result;
    std::ifstream in(physical_path, std::ios::binary);
    if (!in) { result.error = "Failed to open scene file: " + physical_path.string(); return result; }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::istringstream iss(content);
    std::string line;
    if (!std::getline(iss, line) || line.rfind("# NOVAForge Scene",0)!=0) { result.error = "Invalid scene header"; return result; }
    if (!std::getline(iss, line)) { result.error="Missing version"; return result; }
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { result.error="Invalid version line"; return result; }
        std::string vstr = trim(line.substr(pos+1));
        int version = 0;
        try { version = std::stoi(vstr); } catch(...) { result.error="Invalid version number"; return result; }
        if (version != 1) { result.error = "Unsupported scene version: " + vstr + " (expected 1)"; return result; }
    }
    std::string scene_name;
    if (!std::getline(iss, line)) { result.error="Missing name"; return result; }
    {
        auto pos = line.find(":");
        if (pos!=std::string::npos) scene_name = trim(line.substr(pos+1));
    }
    if (!std::getline(iss, line)) { result.error="Missing entity_count"; return result; }
    size_t entity_count=0;
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { result.error="Invalid entity_count"; return result; }
        try { entity_count = static_cast<size_t>(std::stoul(trim(line.substr(pos+1)))); } catch(...) { result.error="Invalid entity_count number"; return result; }
    }
    auto scene = std::make_unique<scene::Scene>(scene_name);
    scene->metadata().version = "1";
    for (size_t i=0;i<entity_count;++i){
        if (!std::getline(iss, line) || trim(line) != "---") { result.error="Expected '---' separator"; return result; }
        if (!std::getline(iss, line) || line.rfind("entity:",0)!=0) { result.error="Expected entity line"; return result; }
        std::string rest = trim(line.substr(7));
        size_t colon = rest.find(":");
        if (colon==std::string::npos) { result.error="Invalid entity line"; return result; }
        ecs::Entity e = scene->world().create_entity();
        std::streampos next_pos;
        while (true) {
            next_pos = iss.tellg();
            if (!std::getline(iss, line)) break;
            std::string trimmed = trim(line);
            if (trimmed.empty()) continue;
            if (trimmed=="---" || trimmed.rfind("entity:",0)==0) { iss.seekg(next_pos); break; }
            if (line.rfind("  Name:",0)==0) {
                std::string nm = trim(line.substr(7));
                if (!nm.empty()) scene->world().add<scene::NameComponent>(e, scene::NameComponent{nm});
            } else if (line.rfind("  Prefab:",0)==0) {
                size_t pp = line.find("path=");
                if (pp != std::string::npos) {
                    std::string p = trim(line.substr(pp + 5));
                    if (!p.empty()) {
                        scene->world().add<scene::PrefabLinkComponent>(
                            e, scene::PrefabLinkComponent{p});
                    }
                }
            } else if (line.rfind("  Transform:",0)==0) {
                float lx=0,ly=0,lz=0, wx=0,wy=0,wz=0;
                int pid=-1, pgen=0;
                // Parent is located by substring: the line may carry optional
                // rot()/scale() segments between world() and parent().
                sscanf(line.c_str(), "  Transform: local(%f,%f,%f) world(%f,%f,%f)", &lx,&ly,&lz,&wx,&wy,&wz);
                size_t ppos = line.find("parent(");
                if (ppos != std::string::npos) {
                    sscanf(line.c_str()+ppos, "parent(%d:%d)", &pid, &pgen);
                }
                auto& trans = scene->world().add<scene::Transform>(e);
                trans.local_x=lx; trans.local_y=ly; trans.local_z=lz;
                trans.world_x=wx; trans.world_y=wy; trans.world_z=wz;
                size_t rpos = line.find("rot(");
                if (rpos != std::string::npos) {
                    float rx=0,ry=0,rz=0;
                    if (sscanf(line.c_str()+rpos, "rot(%f,%f,%f)", &rx,&ry,&rz)==3) {
                        trans.rot_x=rx; trans.rot_y=ry; trans.rot_z=rz;
                    }
                }
                size_t spos = line.find("scale(");
                if (spos != std::string::npos) {
                    float sx=1,sy=1,sz=1;
                    if (sscanf(line.c_str()+spos, "scale(%f,%f,%f)", &sx,&sy,&sz)==3) {
                        if (sx != 0.0f && sy != 0.0f && sz != 0.0f) {
                            trans.scale_x=sx; trans.scale_y=sy; trans.scale_z=sz;
                        }
                    }
                }
                if (pid>=0 && pid != static_cast<int>(0xFFFFFFFF)) {
                    trans.parent = ecs::Entity{static_cast<u32>(pid), static_cast<u32>(pgen)};
                    if (!scene->world().is_alive(trans.parent)) {
                        result.warnings.push_back("Parent entity " + std::to_string(pid) + " not found for entity " + std::to_string(e.id));
                        trans.parent = ecs::kInvalidEntity;
                    }
                }
            } else if (line.rfind("  Mesh:",0)==0) {
                size_t id_pos = line.find("asset_id=");
                if (id_pos != std::string::npos) {
                    size_t start = id_pos+9;
                    size_t end = line.find(" ", start);
                    std::string id_str = trim(line.substr(start, end-start));
                    auto uuid = UUID::from_string(id_str);
                    if (!uuid.is_valid()) {
                        result.warnings.push_back("Invalid AssetId for Mesh: " + id_str);
                        result.missing_assets.push_back(id_str);
                    } else {
                        MeshComponent comp;
                        comp.mesh_id = assets::AssetId(uuid);
                        size_t mat_pos = line.find("material=");
                        if (mat_pos != std::string::npos) comp.material = trim(line.substr(mat_pos+9));
                        scene->world().add<MeshComponent>(e, comp);
                    }
                }
            } else if (line.rfind("  Light:",0)==0) {
                DirectionalLight light;
                size_t dir_pos = line.find("dir(");
                if (dir_pos != std::string::npos) sscanf(line.c_str()+dir_pos, "dir(%f,%f,%f)", &light.dir_x, &light.dir_y, &light.dir_z);
                size_t col_pos = line.find("color(");
                if (col_pos != std::string::npos) sscanf(line.c_str()+col_pos, "color(%f,%f,%f)", &light.color_r, &light.color_g, &light.color_b);
                size_t int_pos = line.find("intensity=");
                if (int_pos != std::string::npos) sscanf(line.c_str()+int_pos, "intensity=%f", &light.intensity);
                scene->world().add<DirectionalLight>(e, light);
            } else if (line.rfind("  Camera:",0)==0) {
                CameraComponent cam;
                size_t fov_pos = line.find("fov=");
                if (fov_pos != std::string::npos) sscanf(line.c_str()+fov_pos, "fov=%f", &cam.fov_y);
                size_t asp_pos = line.find("aspect=");
                if (asp_pos != std::string::npos) sscanf(line.c_str()+asp_pos, "aspect=%f", &cam.aspect);
                size_t near_pos = line.find("near=");
                if (near_pos != std::string::npos) sscanf(line.c_str()+near_pos, "near=%f", &cam.near_plane);
                size_t far_pos = line.find("far=");
                if (far_pos != std::string::npos) sscanf(line.c_str()+far_pos, "far=%f", &cam.far_plane);
                cam.is_active = line.find("active=true") != std::string::npos;
                scene->world().add<CameraComponent>(e, cam);
            } else {
                result.warnings.push_back("Unknown component line: " + line);
            }
        }
    }
    {
        auto all = scene->world().query<scene::Transform>();
        for (ecs::Entity e : all) {
            std::set<u32> visited;
            ecs::Entity cur = e;
            while (cur.valid()) {
                if (visited.count(cur.id)) {
                    result.error = "Cyclic hierarchy detected at entity " + std::to_string(e.id);
                    return result;
                }
                visited.insert(cur.id);
                auto* t = scene->world().get<scene::Transform>(cur);
                if (!t || !t->parent.valid()) break;
                cur = t->parent;
                if (!scene->world().is_alive(cur)) break;
            }
        }
    }
    scene::propagate_transforms(scene->world());
    result.scene = std::move(scene);
    result.success = true;
    return result;
}

SceneLoadResult load_scene_from_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path) {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) {
        SceneLoadResult res;
        res.error = r.error;
        return res;
    }
    return load_scene_from_physical(r.value);
}

bool save_scene_to_physical(const std::filesystem::path& physical_path, const scene::Scene& scene_obj, std::string& out_error) {
    std::filesystem::path tmp = physical_path;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp.parent_path(), ec);
    if (ec) { out_error = ec.message(); return false; }
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) { out_error = "Failed to open file for writing: " + tmp.string(); return false; }
    out << "# NOVAForge Scene v1\n";
    out << "version: 1\n";
    out << "name: " << scene_obj.name() << "\n";
    auto entities = scene_obj.world().all_entities();
    out << "entity_count: " << entities.size() << "\n";
    for (ecs::Entity e : entities) {
        out << "---\n";
        out << "entity: " << e.id << ":" << e.generation << "\n";
        const auto* n = scene_obj.world().get<scene::NameComponent>(e);
        if (n && !n->name.empty()) {
            out << "  Name: " << n->name << "\n";
        }
        const auto* pl = scene_obj.world().get<scene::PrefabLinkComponent>(e);
        if (pl && !pl->prefab_path.empty()) {
            out << "  Prefab: path=" << pl->prefab_path << "\n";
        }
        const auto* t = scene_obj.world().get<scene::Transform>(e);
        if (t) {
            out << "  Transform: local(" << t->local_x << "," << t->local_y << "," << t->local_z << ") world(" << t->world_x << "," << t->world_y << "," << t->world_z << ") rot(" << t->rot_x << "," << t->rot_y << "," << t->rot_z << ") scale(" << t->scale_x << "," << t->scale_y << "," << t->scale_z << ") parent(" << t->parent.id << ":" << t->parent.generation << ")\n";
        }
        const auto* m = scene_obj.world().get<MeshComponent>(e);
        if (m) {
            out << "  Mesh: asset_id=" << m->mesh_id.to_string() << " material=" << m->material << "\n";
        }
        const auto* l = scene_obj.world().get<DirectionalLight>(e);
        if (l) {
            out << "  Light: type=Directional dir(" << l->dir_x << "," << l->dir_y << "," << l->dir_z << ") color(" << l->color_r << "," << l->color_g << "," << l->color_b << ") intensity=" << l->intensity << "\n";
        }
        const auto* c = scene_obj.world().get<CameraComponent>(e);
        if (c) {
            out << "  Camera: fov=" << c->fov_y << " aspect=" << c->aspect << " near=" << c->near_plane << " far=" << c->far_plane << " active=" << (c->is_active ? "true" : "false") << "\n";
        }
    }
    out.close();
    if (!out) { out_error = "Failed to write scene file"; std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, physical_path, ec);
    if (ec) {
        std::filesystem::remove(physical_path, ec);
        std::filesystem::rename(tmp, physical_path, ec);
        if (ec) { out_error = ec.message(); std::filesystem::remove(tmp, ec); return false; }
    }
    return true;
}

bool save_scene_to_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path, const scene::Scene& scene_obj, std::string& out_error) {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) { out_error = r.error; return false; }
    return save_scene_to_physical(r.value, scene_obj, out_error);
}

} // namespace nf::runtime
