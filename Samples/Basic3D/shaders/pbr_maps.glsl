// pbr_maps.glsl — PBR texture-map evaluation, shared by the gbuffer pass
// (gbuffer.frag) and the forward transparency pass (forward.frag).
//
// This lives in its OWN include rather than brdf.glsl on purpose: brdf.glsl
// declares the frame uniform block and the shadow atlases (set 0 bindings
// 4-6), which exist in the lighting/forward descriptor sets but NOT in the
// gbuffer's material set (where 4-6 are the maps themselves). Including
// brdf.glsl from gbuffer.frag would redeclare those bindings with different
// types — a silent descriptor mismatch. One copy of the map math here, and
// both paths call it, is what keeps the two paths agreeing about what a
// mapped surface is.
//
// map_flags: x = normal, y = metallic-roughness, z = occlusion, w = emissive.
// Samplers ride parameters (the two passes bind their maps at different
// indices: gbuffer 3-6, forward 10-13).
//
// Conventions (glTF, documented in MaterialAsset):
//   normal map    tangent-space, OpenGL orientation (Y+), unpacked *2-1.
//   mrough map    G = roughness multiplier, B = metallic multiplier.
//   occlusion map R = occlusion multiplier on the ao scalar.
//   emissive map  rgb multiplies emission colour x strength.
//
// Normal mapping needs a tangent frame, and the vertex stream carries no
// tangents (adding a vertex attribute is a mesh-format change for every
// importer). The frame is built from screen-space derivatives instead — the
// standard Mikkelsen-style construction: exact for affine-mapped triangles,
// which is everything this rasterizer draws. Front faces only (both passes
// cull back faces), OpenGL-style maps; a DirectX-style (Y-) normal map reads
// vertically flipped, and mirrored UV islands flip the handedness — both are
// authoring conventions no code can detect, so author Y+ unmirrored maps.
//
// No #version here: the including file owns it, and a second occurrence is a
// hard glslc error.

void apply_pbr_maps(sampler2D normal_map, sampler2D mrough_map,
                    sampler2D occlusion_map, sampler2D emissive_map,
                    vec4 map_flags, vec3 world_pos, vec2 uv,
                    inout vec3 N, inout vec3 albedo,
                    inout float metallic, inout float roughness,
                    inout float ao, inout vec3 emission) {
    if (map_flags.x > 0.5) {
        vec3 map_n = texture(normal_map, uv).rgb * 2.0 - 1.0;
        vec3 q0 = dFdx(world_pos);
        vec3 q1 = dFdy(world_pos);
        vec2 st0 = dFdx(uv);
        vec2 st1 = dFdy(uv);
        vec3 Ng = normalize(N);
        // Tangent orientation: T = dP/du must point along +U in the world,
        // or every normal map reads mirrored. The raw solve above
        // (q0*st1.t - q1*st0.t) comes out negated for this pipeline's
        // screen/UV orientation, so it is negated back here. This is pinned
        // empirically, not derived: pbr_normal_map_tilts_the_surface_toward_
        // the_light renders +X and -X tilts under a +X light, and only the
        // negated frame puts the bright side on +X. If UV or framebuffer
        // orientation ever changes, that test is what will say so.
        vec3 T = -(q0 * st1.t - q1 * st0.t);
        // Gram-Schmidt: UV seams shear the derivative frame, and an
        // un-orthogonalised T then leans the perturbed normal sideways.
        T = T - Ng * dot(Ng, T);
        // A mesh with constant UVs (or a solid-colour normal texel under
        // magnification) has no UV gradient: normalising a zero tangent is
        // NaN, so the geometric normal stands in instead of a black pixel.
        float t_len = length(T);
        if (t_len > 1e-6) {
            T /= t_len;
            vec3 B = normalize(cross(Ng, T));
            N = normalize(T * map_n.x + B * map_n.y + Ng * map_n.z);
        }
    }
    if (map_flags.y > 0.5) {
        vec3 mr = texture(mrough_map, uv).rgb;
        roughness = clamp(roughness * clamp(mr.g, 0.0, 1.0), 0.045, 1.0);
        metallic = clamp(metallic * clamp(mr.b, 0.0, 1.0), 0.0, 1.0);
    }
    if (map_flags.z > 0.5) {
        ao = clamp(ao * clamp(texture(occlusion_map, uv).r, 0.0, 1.0), 0.0, 1.0);
    }
    if (map_flags.w > 0.5) {
        emission *= clamp(texture(emissive_map, uv).rgb, vec3(0.0), vec3(64.0));
    }
}
