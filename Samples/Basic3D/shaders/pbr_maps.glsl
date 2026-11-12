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
// normal_scale: glTF `normalTexture.scale` (default 1). Scales only XY.
// occlusion_strength: glTF `occlusionTexture.strength` (default 1). Blends the
// sampled R toward 1; 0 disables the map.
// Samplers ride parameters (the two passes bind their maps at different
// indices: gbuffer 3-6, forward 10-13).
//
// Conventions (glTF 2.0, documented in MaterialAsset):
//   normal map    tangent-space, OpenGL orientation (Y+), unpacked *2-1.
//   mrough map    G = roughness multiplier, B = metallic multiplier.
//   occlusion map R = occlusion multiplier on the ao scalar.
//   emissive map  rgb multiplies emission colour x strength.
//
// TANGENT FRAME (derivation, not an empirical sign)
// -------------------------------------------------
// The vertex stream carries no bound tangent attribute, so the frame is
// recovered from screen-space derivatives. On the tangent plane:
//     q0 = T*st0.s + Bv*st0.t
//     q1 = T*st1.s + Bv*st1.t
// with q0=dFdx(P), q1=dFdy(P), st0=dFdx(uv), st1=dFdy(uv). Solving the
// 2x2 system gives, with det = st0.s*st1.t - st0.t*st1.s:
//     T  = (st1.t*q0 - st0.t*q1) / det    (direction of increasing U)
//     Bv = (st0.s*q1 - st1.s*q0) / det    (direction of increasing V)
//
// `det` MUST NOT be dropped. Vulkan's Y-flipped projection, glTF's V-DOWN
// UVs, engine-authored V-UP UVs and mirrored-U islands all give different
// determinant signs. An earlier version hardcoded `T = -(st1.t*q0 - st0.t*q1)`
// and `B = cross(N,T)`, which is equivalent to assuming det = -1; that is the
// V-UP native-cube case (StaticMesh::create_cube) but INVERTS both axes for
// standard glTF assets (det = +1). It also loses mirrored-UV handedness.
//
// B = -Bv because Vulkan/glTF textures have a top-left origin (V increases
// DOWNWARD) while OpenGL-style/glTF normal maps define green as pointing
// toward the TOP of the image, i.e. toward DECREASING V. This single rule is
// convention-independent: it produces the same green axis for V-up native
// meshes and V-down glTF meshes, and it is exactly the handedness that
// glTF's TANGENT.w encodes. Mirrored U islands flip det and therefore T,
// which is what makes their tangent-space detail mirror correctly.
//
// No #version here: the including file owns it, and a second occurrence is a
// hard glslc error.

void apply_pbr_maps(sampler2D normal_map, sampler2D mrough_map,
                    sampler2D occlusion_map, sampler2D emissive_map,
                    vec4 map_flags, float normal_scale, float occlusion_strength,
                    vec3 world_pos, vec2 uv,
                    inout vec3 N, inout vec3 albedo,
                    inout float metallic, inout float roughness,
                    inout float ao, inout vec3 emission) {
    if (map_flags.x > 0.5) {
        vec3 map_n = texture(normal_map, uv).rgb * 2.0 - 1.0;
        // glTF normalTexture.scale multiplies the tangent-plane components.
        // Renormalising afterwards keeps the decoded normal on the unit
        // sphere (scale 0 must collapse exactly to the geometric normal).
        map_n.xy *= normal_scale;
        map_n = normalize(map_n);

        vec3 q0 = dFdx(world_pos);
        vec3 q1 = dFdy(world_pos);
        vec2 st0 = dFdx(uv);
        vec2 st1 = dFdy(uv);
        vec3 Ng = normalize(N);
        float det = st0.x * st1.y - st0.y * st1.x;

        // Constant UVs, a mirrored seam or a degenerate projection give no
        // usable Jacobian; keeping the geometric normal avoids NaN/black
        // pixels instead of normalising a zero vector.
        if (abs(det) > 1e-8) {
            float inv_det = 1.0 / det;
            vec3 T = (st1.y * q0 - st0.y * q1) * inv_det;
            vec3 B = (st1.x * q0 - st0.x * q1) * inv_det; // -Bv, see above

            // Gram-Schmidt: UV seams shear the derivative frame, and an
            // un-orthogonalised T or B leans the perturbed normal sideways.
            T -= Ng * dot(Ng, T);
            B -= Ng * dot(Ng, B);
            const float t2 = dot(T, T);
            const float b2 = dot(B, B);
            if (t2 > 1e-12 && b2 > 1e-12) {
                T *= inversesqrt(t2);
                B *= inversesqrt(b2);
                N = normalize(T * map_n.x + B * map_n.y + Ng * map_n.z);
            }
        }
    }
    if (map_flags.y > 0.5) {
        vec3 mr = texture(mrough_map, uv).rgb;
        roughness = clamp(roughness * clamp(mr.g, 0.0, 1.0), 0.045, 1.0);
        metallic = clamp(metallic * clamp(mr.b, 0.0, 1.0), 0.0, 1.0);
    }
    if (map_flags.z > 0.5) {
        // glTF occlusion: mix(1, r, strength). strength 1 = full map (the old
        // behaviour), 0 = no occlusion; intermediate values blend toward 1.
        float occ = clamp(texture(occlusion_map, uv).r, 0.0, 1.0);
        ao = clamp(ao * mix(1.0, occ, clamp(occlusion_strength, 0.0, 1.0)), 0.0, 1.0);
    }
    if (map_flags.w > 0.5) {
        emission *= clamp(texture(emissive_map, uv).rgb, vec3(0.0), vec3(64.0));
    }
}
