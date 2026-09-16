#define _CRT_SECURE_NO_WARNINGS
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Audio/AudioDecoder.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Core/Logger.hpp>
#include <fstream>
#include <set>
#include <sstream>
#include <cstring>
#include <unordered_map>

namespace nf::runtime {

static std::string trim(const std::string& s) {
    size_t a=0; while(a<s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b=s.size(); while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}

// Value of a `key=value` token on a component line, up to the next space.
// Returns "" when the key is absent. `key` must include the '='.
static std::string field_value(const std::string& line, const char* key) {
    const size_t p = line.find(key);
    if (p == std::string::npos) return {};
    const size_t start = p + std::strlen(key);
    const size_t end = line.find(' ', start);
    return trim(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

// As above, parsed as a float. Returns false (leaving `out` untouched) when the
// key is absent or the value is not a number, so callers keep their defaults
// instead of silently reading a zero.
static bool field_float(const std::string& line, const char* key, f32& out) {
    const std::string v = field_value(line, key);
    if (v.empty()) return false;
    try {
        size_t used = 0;
        const f32 parsed = std::stof(v, &used);
        if (used != v.size()) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

// Axis back to the single-letter form the loader reads. Only the three
// cardinal axes are writable; anything else collapses to Y, which matches the
// loader's own default and keeps save/load a fixed point.
static const char* axis_name(const Vec3& axis) {
    if (axis.x > 0.5f) return "x";
    if (axis.z > 0.5f) return "z";
    return "y";
}

// --- Module property encoding ----------------------------------------------
//
// A component line is a sequence of space-separated `key=value` tokens, and
// `field_value` stops at the first space. Module state is user data, so the
// values are escaped rather than assumed clean — without that, `label=hello
// world` would silently truncate to `hello` on the round trip.
//
// The encoding itself lives in NF/Gameplay/GameplayState.hpp, shared with the
// save system. A second copy here would be free to drift, and the failure mode
// of a drifted escape is silent data corruption rather than an error.

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
                // Absent key keeps the default (true): old scene files load lit as before.
                if (line.find("shadows=false") != std::string::npos) light.cast_shadows = false;
                size_t ss_pos = line.find("shadow_strength=");
                if (ss_pos != std::string::npos) sscanf(line.c_str()+ss_pos, "shadow_strength=%f", &light.shadow_strength);
                size_t sb_pos = line.find("shadow_bias=");
                if (sb_pos != std::string::npos) sscanf(line.c_str()+sb_pos, "shadow_bias=%f", &light.shadow_bias);
                scene->world().add<DirectionalLight>(e, light);
            } else if (line.rfind("  Sky:",0)==0) {
                SkyComponent sky;
                size_t zp = line.find("zenith(");
                if (zp != std::string::npos) sscanf(line.c_str()+zp, "zenith(%f,%f,%f)", &sky.zenith_r, &sky.zenith_g, &sky.zenith_b);
                size_t hp = line.find("horizon(");
                if (hp != std::string::npos) sscanf(line.c_str()+hp, "horizon(%f,%f,%f)", &sky.horizon_r, &sky.horizon_g, &sky.horizon_b);
                size_t gp = line.find("ground(");
                if (gp != std::string::npos) sscanf(line.c_str()+gp, "ground(%f,%f,%f)", &sky.ground_r, &sky.ground_g, &sky.ground_b);
                size_t cp = line.find("clear(");
                if (cp != std::string::npos) sscanf(line.c_str()+cp, "clear(%f,%f,%f)", &sky.clear_r, &sky.clear_g, &sky.clear_b);
                size_t dp = line.find("sun_disk=");
                if (dp != std::string::npos) sscanf(line.c_str()+dp, "sun_disk=%f", &sky.sun_disk);
                size_t glp = line.find("sun_glow=");
                if (glp != std::string::npos) sscanf(line.c_str()+glp, "sun_glow=%f", &sky.sun_glow);
                // Absent key keeps the default (true): old scene files keep the sky.
                if (line.find("enabled=false") != std::string::npos) sky.enabled = false;
                scene->world().add<SkyComponent>(e, sky);
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
            } else if (line.rfind("  RigidBody:",0)==0) {
                physics::RigidBodyComponent rb;
                if (line.find("type=Static") != std::string::npos) {
                    rb.type = physics::BodyType::Static;
                } else if (line.find("type=Kinematic") != std::string::npos) {
                    rb.type = physics::BodyType::Kinematic;
                }
                // Parsed by substring rather than one big sscanf: the fields are
                // all optional and a missing one must leave the default in place,
                // not fail the whole line.  We search for the key prefix (e.g.
                // "mass=") and then sscanf the value from that offset — the format
                // string contains the prefix so sscanf matches it literally.
                auto scan = [&](const char* prefix, f32& out) {
                    const size_t p = line.find(prefix);
                    if (p != std::string::npos) {
                        sscanf(line.c_str() + p + std::strlen(prefix), "%f", &out);
                    }
                };
                scan("mass=", rb.mass);
                scan("friction=", rb.friction);
                scan("restitution=", rb.restitution);
                scan("linear_damping=", rb.linear_damping);
                scan("angular_damping=", rb.angular_damping);
                rb.allow_sleep = line.find("allow_sleep=false") == std::string::npos;
                scene->world().add<physics::RigidBodyComponent>(e, rb);
            } else if (line.rfind("  Collider:",0)==0) {
                physics::ColliderComponent col;
                if (line.find("shape=Box") != std::string::npos) {
                    float hx=0.5f, hy=0.5f, hz=0.5f;
                    const size_t hp = line.find("half(");
                    if (hp != std::string::npos) {
                        sscanf(line.c_str()+hp, "half(%f,%f,%f)", &hx,&hy,&hz);
                    }
                    col.shape = physics::Shape::make_box(Vec3(hx,hy,hz));
                } else if (line.find("shape=Plane") != std::string::npos) {
                    float nx=0.0f, ny=1.0f, nz=0.0f;
                    const size_t np = line.find("normal(");
                    if (np != std::string::npos) {
                        sscanf(line.c_str()+np, "normal(%f,%f,%f)", &nx,&ny,&nz);
                    }
                    col.shape = physics::Shape::make_plane(Vec3(nx,ny,nz));
                } else { // Sphere is the default shape
                    float r = 0.5f;
                    const size_t rp = line.find("radius=");
                    if (rp != std::string::npos) {
                        sscanf(line.c_str()+rp, "radius=%f", &r);
                    }
                    col.shape = physics::Shape::make_sphere(r);
                }
                scene->world().add<physics::ColliderComponent>(e, col);
            } else if (line.rfind("  Animation:",0)==0) {
                animation::AnimationComponent anim;

                const std::string clip_name = field_value(line, "clip=");
                if (!clip_name.empty()) {
                    anim.player.set_clip(clip_name);
                }

                // `procedural=` names a clip the engine builds itself. Without
                // it a clip name is only a label: the import pipeline is an
                // explicit Phase 9 non-goal (§7), so there is nothing on disk to
                // load and the entity would sample the rest pose forever.
                const std::string procedural = field_value(line, "procedural=");
                if (!procedural.empty() && !clip_name.empty()) {
                    animation::ProceduralClipSpec spec;
                    spec.kind = (procedural == "bob") ? animation::ProceduralClipSpec::Kind::Bob
                                                      : animation::ProceduralClipSpec::Kind::Spin;
                    const std::string axis = field_value(line, "axis=");
                    if (axis == "x") {
                        spec.axis = {1.0f, 0.0f, 0.0f};
                    } else if (axis == "z") {
                        spec.axis = {0.0f, 0.0f, 1.0f};
                    } else {
                        spec.axis = {0.0f, 1.0f, 0.0f};
                    }
                    (void)field_float(line, "turns=", spec.turns);
                    (void)field_float(line, "amplitude=", spec.amplitude);
                    (void)field_float(line, "duration=", spec.duration);

                    // No skeleton import pipeline either, so a procedural clip
                    // gets the minimal single-bone rig. Logged rather than
                    // silent: a real rig would give a different result, and a
                    // substitution the user cannot see is the kind of gap this
                    // project keeps having to re-find.
                    if (anim.skeleton.bones.empty()) {
                        anim.skeleton = animation::make_default_skeleton();
                        NF_LOG_INFO(LogCategory::Core,
                                    "Runtime: entity {} has no skeleton data; using a single-bone root rig for procedural clip '{}'",
                                    e.id, clip_name);
                    }

                    anim.clips[clip_name] =
                        animation::make_procedural_clip(clip_name, anim.skeleton, spec);
                    anim.has_procedural = true;
                    anim.procedural = spec;
                    anim.procedural_clip_name = clip_name;
                } else if (!clip_name.empty()) {
                    result.warnings.push_back(
                        "Animation clip '" + clip_name +
                        "' has no data (no procedural= spec, and there is no animation import pipeline yet); the entity will not move");
                }

                (void)field_float(line, "speed=", anim.speed);
                anim.player.set_speed(anim.speed);

                const std::string loop = field_value(line, "loop=");
                if (loop == "none") {
                    anim.player.set_loop_mode(animation::LoopMode::None);
                } else if (loop == "pingpong") {
                    anim.player.set_loop_mode(animation::LoopMode::PingPong);
                } else {
                    anim.player.set_loop_mode(animation::LoopMode::Loop);
                }

                anim.paused = line.find("paused=true") != std::string::npos;
                anim.use_state_machine = line.find("state_machine=true") != std::string::npos;
                // An AnimationComponent in a scene plays unless it says it is
                // paused. AnimationPlayer::update() returns the current time
                // unchanged unless the state is Playing, so without this a
                // loaded scene would sit at t = 0 forever and the entity would
                // look static even with a valid clip.
                if (!anim.paused) {
                    anim.player.play();
                }
                scene->world().add<animation::AnimationComponent>(e, std::move(anim));
            } else if (line.rfind("  Audio:",0)==0) {
                audio::AudioComponent aud;
                aud.buffer_name = field_value(line, "buffer=");

                // Same reasoning as the animation clip above: `tone=` is the
                // quick procedural path. A `buffer=` logical path (e.g.
                // content://Audio/shoot.wav) resolves through the VFS import
                // pipeline after load (see resolve_scene_audio below).
                f32 tone_hz = 0.0f;
                if (field_float(line, "tone=", tone_hz) && tone_hz > 0.0f) {
                    f32 tone_seconds = 0.5f;
                    (void)field_float(line, "tone_duration=", tone_seconds);
                    aud.owned_buffer =
                        audio::make_tone_buffer(tone_hz, tone_seconds, audio::kDefaultSampleRate, 1);
                    aud.tone_hz = tone_hz;
                    aud.tone_duration = tone_seconds;
                }

                (void)field_float(line, "volume=", aud.volume);
                (void)field_float(line, "pitch=", aud.pitch);
                aud.looping = line.find("looping=true") != std::string::npos;
                aud.spatial = line.find("spatial=true") != std::string::npos;
                aud.autoplay = line.find("autoplay=true") != std::string::npos;
                scene->world().add<audio::AudioComponent>(e, std::move(aud));
            } else if (line.rfind("  Module:",0)==0) {
                gameplay::GameplayModuleComponent comp;
                comp.module_name = field_value(line, "name=");
                comp.enabled = line.find("enabled=false") == std::string::npos;

                // The state is whatever the module's class declared as
                // SerializeField; the loader does not need to know the type.
                const std::string encoded = field_value(line, "props=");
                if (!encoded.empty()) {
                    gameplay::decode_properties(encoded, comp.properties);
                }

                if (comp.module_name.empty()) {
                    // A nameless module cannot be matched to a factory, so it is
                    // reported rather than stored as an orphan component.
                    result.warnings.push_back("Module line without a name was ignored: " + line);
                } else {
                    scene->world().add<gameplay::GameplayModuleComponent>(e, std::move(comp));
                }
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
    SceneLoadResult res = load_scene_from_physical(r.value);
    if (res.success && res.scene) {
        resolve_scene_audio(vfs, *res.scene, res.warnings);
    }
    return res;
}

// Binds AudioComponent::buffer_name through the VFS import pipeline:
// reads the file and decodes it (WAV/OGG/MP3/FLAC) into owned_buffer at the
// mix rate, so the source is audible without any procedural tone. Unresolvable
// names keep the old loud warning instead of silent failure.
void resolve_scene_audio(assets::VirtualFileSystem& vfs, scene::Scene& scene_obj,
                         std::vector<std::string>& warnings) {
    for (ecs::Entity e : scene_obj.world().all_entities()) {
        auto* aud = scene_obj.world().get<audio::AudioComponent>(e);
        if (!aud || !aud->owned_buffer.samples.empty() || aud->buffer_name.empty()) continue;
        auto bytes = vfs.read_bytes(aud->buffer_name);
        if (!bytes.ok) {
            warnings.push_back("Audio buffer '" + aud->buffer_name +
                               "' not found in VFS; the source will be silent");
            continue;
        }
        audio::DecodeOptions options;
        options.target_sample_rate = audio::kDefaultSampleRate;
        audio::DecodeResult decoded =
            audio::decode_audio_memory(bytes.value.data(), bytes.value.size(), options);
        if (!decoded.ok) {
            warnings.push_back("Audio buffer '" + aud->buffer_name + "' could not be decoded (" +
                               decoded.error + "); the source will be silent");
            continue;
        }
        aud->owned_buffer = std::move(decoded.buffer);
    }
}

std::string serialize_scene_to_text(const scene::Scene& scene_obj) {
    std::ostringstream out;
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
            out << "  Light: type=Directional dir(" << l->dir_x << "," << l->dir_y << "," << l->dir_z << ") color(" << l->color_r << "," << l->color_g << "," << l->color_b << ") intensity=" << l->intensity;
            if (!l->cast_shadows) out << " shadows=false";
            // Non-default shadow tuning is explicit so old files (which omit
            // both keys) keep rendering exactly as before.
            if (l->shadow_strength != 1.0f) out << " shadow_strength=" << l->shadow_strength;
            if (l->shadow_bias != 0.0005f) out << " shadow_bias=" << l->shadow_bias;
            out << "\n";
        }
        const auto* sky = scene_obj.world().get<SkyComponent>(e);
        if (sky) {
            out << "  Sky: zenith(" << sky->zenith_r << "," << sky->zenith_g << "," << sky->zenith_b << ")"
                << " horizon(" << sky->horizon_r << "," << sky->horizon_g << "," << sky->horizon_b << ")"
                << " ground(" << sky->ground_r << "," << sky->ground_g << "," << sky->ground_b << ")"
                << " clear(" << sky->clear_r << "," << sky->clear_g << "," << sky->clear_b << ")"
                << " sun_disk=" << sky->sun_disk << " sun_glow=" << sky->sun_glow
                << " enabled=" << (sky->enabled ? "true" : "false") << "\n";
        }
        const auto* c = scene_obj.world().get<CameraComponent>(e);
        if (c) {
            out << "  Camera: fov=" << c->fov_y << " aspect=" << c->aspect << " near=" << c->near_plane << " far=" << c->far_plane << " active=" << (c->is_active ? "true" : "false") << "\n";
        }
        // Physics. The runtime body handle is deliberately NOT written: it is a
        // per-session identity (generation-checked), and a stale one from a
        // previous run is meaningless — reloading rebuilds it from the scene.
        const auto* rb = scene_obj.world().get<physics::RigidBodyComponent>(e);
        if (rb) {
            const char* type = (rb->type == physics::BodyType::Static) ? "Static"
                             : (rb->type == physics::BodyType::Kinematic) ? "Kinematic"
                                                                          : "Dynamic";
            out << "  RigidBody: type=" << type
                << " mass=" << rb->mass
                << " friction=" << rb->friction
                << " restitution=" << rb->restitution
                << " linear_damping=" << rb->linear_damping
                << " angular_damping=" << rb->angular_damping
                << " allow_sleep=" << (rb->allow_sleep ? "true" : "false") << "\n";
        }
        const auto* col = scene_obj.world().get<physics::ColliderComponent>(e);
        if (col) {
            if (col->shape.type == physics::ShapeType::Box) {
                out << "  Collider: shape=Box half("
                    << col->shape.box.half_extents.x << ","
                    << col->shape.box.half_extents.y << ","
                    << col->shape.box.half_extents.z << ")\n";
            } else if (col->shape.type == physics::ShapeType::Plane) {
                out << "  Collider: shape=Plane normal("
                    << col->shape.plane.normal.x << ","
                    << col->shape.plane.normal.y << ","
                    << col->shape.plane.normal.z << ")\n";
            } else {
                out << "  Collider: shape=Sphere radius=" << col->shape.sphere.radius << "\n";
            }
        }
        const auto* anim = scene_obj.world().get<animation::AnimationComponent>(e);
        if (anim) {
            const char* loop = (anim->player.loop_mode() == animation::LoopMode::None) ? "none"
                             : (anim->player.loop_mode() == animation::LoopMode::PingPong) ? "pingpong"
                                                                                          : "loop";
            out << "  Animation: clip=" << anim->player.clip_name()
                << " speed=" << anim->speed
                << " loop=" << loop
                << " paused=" << (anim->paused ? "true" : "false")
                << " state_machine=" << (anim->use_state_machine ? "true" : "false");
            // The clip itself is generated at load time, never stored, so the
            // spec is the only thing that can rebuild it. Writing only the clip
            // name would reload the scene with an empty clip and the entity
            // would silently stop moving.
            if (anim->has_procedural) {
                out << " procedural="
                    << (anim->procedural.kind == animation::ProceduralClipSpec::Kind::Bob ? "bob"
                                                                                         : "spin")
                    << " axis=" << axis_name(anim->procedural.axis)
                    << " turns=" << anim->procedural.turns
                    << " amplitude=" << anim->procedural.amplitude
                    << " duration=" << anim->procedural.duration;
            }
            out << "\n";
        }
        const auto* aud = scene_obj.world().get<audio::AudioComponent>(e);
        if (aud) {
            out << "  Audio: buffer=" << aud->buffer_name
                << " volume=" << aud->volume
                << " pitch=" << aud->pitch
                << " looping=" << (aud->looping ? "true" : "false")
                << " spatial=" << (aud->spatial ? "true" : "false")
                << " autoplay=" << (aud->autoplay ? "true" : "false");
            // Same as the animation spec above: the samples are generated, so
            // the spec is what makes them survive a save/load.
            if (aud->tone_hz > 0.0f) {
                out << " tone=" << aud->tone_hz << " tone_duration=" << aud->tone_duration;
            }
            out << "\n";
        }
        const auto* mod = scene_obj.world().get<gameplay::GameplayModuleComponent>(e);
        if (mod && !mod->module_name.empty()) {
            out << "  Module: name=" << mod->module_name
                << " enabled=" << (mod->enabled ? "true" : "false")
                << " props=" << gameplay::encode_properties(mod->properties)
                << "\n";
        }
    }
    return out.str();
}

bool save_scene_to_physical(const std::filesystem::path& physical_path, const scene::Scene& scene_obj, std::string& out_error) {
    const std::string text = serialize_scene_to_text(scene_obj);

    std::filesystem::path tmp = physical_path;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp.parent_path(), ec);
    if (ec) { out_error = ec.message(); return false; }

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { out_error = "Failed to open file for writing: " + tmp.string(); return false; }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) { out_error = "Failed to write scene file"; std::filesystem::remove(tmp, ec); return false; }
    }

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

void copy_scene_entity(const ecs::World& src, ecs::Entity se, ecs::World& dst, ecs::Entity de) {
    if (const auto* t = src.get<scene::Transform>(se)) {
        dst.add<scene::Transform>(de, *t);
    }
    if (const auto* n = src.get<scene::NameComponent>(se)) {
        dst.add<scene::NameComponent>(de, *n);
    }
    if (const auto* p = src.get<scene::PrefabLinkComponent>(se)) {
        dst.add<scene::PrefabLinkComponent>(de, *p);
    }
    if (const auto* m = src.get<MeshComponent>(se)) {
        dst.add<MeshComponent>(de, *m);
    }
    if (const auto* l = src.get<DirectionalLight>(se)) {
        dst.add<DirectionalLight>(de, *l);
    }
    if (const auto* s = src.get<SkyComponent>(se)) {
        dst.add<SkyComponent>(de, *s);
    }
    if (const auto* c = src.get<CameraComponent>(se)) {
        dst.add<CameraComponent>(de, *c);
    }
    if (const auto* rb = src.get<physics::RigidBodyComponent>(se)) {
        physics::RigidBodyComponent fresh = *rb;
        // A live body handle is per-session, never scene data: copying it
        // would alias two entities onto one body (or a dead one). The
        // runtime rebuilds bodies from the components after a merge/load.
        fresh.body = physics::BodyHandle{};
        dst.add<physics::RigidBodyComponent>(de, fresh);
    }
    if (const auto* col = src.get<physics::ColliderComponent>(se)) {
        dst.add<physics::ColliderComponent>(de, *col);
    }
    if (const auto* a = src.get<animation::AnimationComponent>(se)) {
        dst.add<animation::AnimationComponent>(de, *a);
    }
    if (const auto* au = src.get<audio::AudioComponent>(se)) {
        dst.add<audio::AudioComponent>(de, *au);
    }
    if (const auto* g = src.get<gameplay::GameplayModuleComponent>(se)) {
        dst.add<gameplay::GameplayModuleComponent>(de, *g);
    }
}

namespace {

std::vector<ecs::Entity> collect_members(const ecs::World& world, ecs::Entity root) {
    std::vector<ecs::Entity> out;
    if (!root.valid() || !world.is_alive(root)) {
        return out;
    }
    // Iterative pre-order DFS (parent before children).
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty()) {
        ecs::Entity cur = stack.back();
        stack.pop_back();
        if (!world.is_alive(cur)) {
            continue;
        }
        out.push_back(cur);
        auto kids = scene::get_children(world, cur);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return out;
}

} // namespace

SceneMergeResult merge_scene_into_world(assets::VirtualFileSystem& vfs, const std::string& logical_path,
                                        ecs::World& dst_world) {
    SceneMergeResult out;
    SceneLoadResult loaded = load_scene_from_vfs(vfs, logical_path);
    if (!loaded.success || !loaded.scene) {
        out.error = loaded.error.empty() ? ("Cannot merge scene '" + logical_path + "'") : loaded.error;
        return out;
    }
    out.created = merge_loaded_scene_into_world(*loaded.scene, dst_world);
    out.warnings = loaded.warnings;
    for (const std::string& missing : loaded.missing_assets) {
        out.warnings.push_back("Missing asset: " + missing);
    }
    out.success = true;
    return out;
}

std::vector<ecs::Entity> merge_loaded_scene_into_world(scene::Scene& chunk, ecs::World& dst_world) {
    std::vector<ecs::Entity> created;
    const ecs::World& src = chunk.world();

    // Roots: entities with no live parent in the chunk (same rule as prefab
    // templates). Internal hierarchy is remapped below; chunk roots become
    // parentless members of the live world.
    std::vector<ecs::Entity> roots;
    for (ecs::Entity e : src.all_entities()) {
        const auto* t = src.get<scene::Transform>(e);
        if (t == nullptr || !t->parent.valid() || !src.is_alive(t->parent)) {
            roots.push_back(e);
        }
    }

    std::unordered_map<u32, ecs::Entity> remap;
    for (ecs::Entity root : roots) {
        const std::vector<ecs::Entity> members = collect_members(src, root);
        if (members.empty()) {
            continue;
        }
        ecs::Entity new_root;
        bool first = true;
        for (ecs::Entity se : members) {
            ecs::Entity de = dst_world.create_entity();
            remap[se.id] = de;
            copy_scene_entity(src, se, dst_world, de);
            // Chunk roots start parentless (their file parent, if any, did
            // not survive the liveness check above); the remap pass below
            // only rewires members below their own root.
            if (auto* dt = dst_world.get<scene::Transform>(de)) {
                dt->parent = ecs::kInvalidEntity;
                dt->dirty = true;
            }
            if (first) {
                new_root = de;
                first = false;
            }
        }
        for (ecs::Entity se : members) {
            if (se == root) {
                continue;
            }
            const auto* st = src.get<scene::Transform>(se);
            if (st == nullptr || !st->parent.valid()) {
                continue;
            }
            const auto it = remap.find(st->parent.id);
            if (it != remap.end()) {
                scene::set_parent(dst_world, remap[se.id], it->second);
            }
        }
        if (new_root.valid()) {
            created.push_back(new_root);
        }
    }

    scene::propagate_transforms(dst_world);
    return created;
}

} // namespace nf::runtime
