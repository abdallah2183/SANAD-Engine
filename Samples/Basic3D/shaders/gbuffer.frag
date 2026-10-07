#version 450

// GBuffer fragment shader — writes the deferred material surface:
//   attachment 0: BaseColor (rgba)
//   attachment 1: Normal (encoded *0.5+0.5)
//   attachment 2: Metallic (r), Roughness (g), AO (b), EmissionStrength (a)
//   attachment 3: Emissive radiance (emission.rgb * EmissionStrength, rgb)
//
// Material parameters arrive through a per-instance uniform block (binding 0)
// so changing them never requires a new pipeline. An optional albedo texture
// (binding 1) multiplies the scalar base color when enabled. The terrain
// layer palette (binding 2) supplies the flat colours of the layers above the
// base material — see the splat block in main().

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_uv1;
layout(location = 3) in vec3 in_world_pos;

layout(location = 0) out vec4 out_base_color;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_surface;
layout(location = 3) out vec4 out_emissive;

layout(set = 0, binding = 0) uniform MaterialParams {
    vec4 baseColor;   // rgb + a (alpha kept for future transparency)
    vec4 surface;     // metallic, roughness, ao, emissionStrength
    vec4 misc;        // emission.rgb, useBaseColorTex
    vec4 mapFlags;    // useNormalTex, useMRoughTex, useOcclusionTex, useEmissiveTex
} matParams;

layout(set = 0, binding = 1) uniform sampler2D albedoTex;
// PBR maps (bindings 3-6 of the material layout). Unbound slots read the
// renderer's white fallback, and the mapFlags guard means the substitute is
// never sampled — the same rule the albedo binding follows.
layout(set = 0, binding = 3) uniform sampler2D normalTex;
layout(set = 0, binding = 4) uniform sampler2D mroughTex;
layout(set = 0, binding = 5) uniform sampler2D occlusionTex;
layout(set = 0, binding = 6) uniform sampler2D emissiveTex;

// Must match Renderer3D::kSplatPaletteLayers. Slot 0 is never read from here —
// layer 0 is the material itself — but is kept so `slot + 1` stays in range
// for the top band.
const int kSplatPaletteLayers = 8;

layout(set = 0, binding = 2) uniform SplatPalette {
    vec4 layers[kSplatPaletteLayers];
} splatPalette;

#include "pbr_maps.glsl"

void main() {
    vec3 n = normalize(in_normal);
    vec3 base = matParams.baseColor.rgb;
    if (matParams.misc.w > 0.5) {
        base *= texture(albedoTex, in_uv).rgb;
    }
    float metallic = matParams.surface.x;
    float roughness = matParams.surface.y;
    float ao = matParams.surface.z;
    vec3 emission = matParams.misc.rgb * matParams.surface.w;
    // Texture maps modulate the scalars above (multiplicative, glTF-style),
    // evaluated here so the gbuffer stores the FINAL surface — the lighting
    // pass never knows whether a value was authored or sampled.
    apply_pbr_maps(normalTex, mroughTex, occlusionTex, emissiveTex,
                   matParams.mapFlags, in_world_pos, in_uv,
                   n, base, metallic, roughness, ao, emission);

    // Terrain layer splat. A surface the mesh has classified carries
    // `uv1 = (slot, blend)`: the layer it is in, and how far it has moved
    // into the slot above. Layer 0 is the material below — the terrain
    // builder's contract is that a surface with no layers renders as the
    // base texture — so only slots >= 1 come from the palette, and a vertex
    // at slot 0 blends the material toward palette[1]. (0, 0) is an
    // unclassified surface (uv1 is optional and defaults to it), which is why
    // the guard is a value test and not a per-object flag: the material below
    // is taken untouched and every pixel authored against it still holds.
    if (in_uv1.x > 0.5 || in_uv1.y > 0.5) {
        int slot = clamp(int(in_uv1.x + 0.5), 0, kSplatPaletteLayers - 1);
        int next = min(slot + 1, kSplatPaletteLayers - 1);
        vec3 layer0 = slot == 0 ? base : splatPalette.layers[slot].rgb;
        base = mix(layer0, splatPalette.layers[next].rgb, clamp(in_uv1.y, 0.0, 1.0));
    }

    out_base_color = vec4(base, matParams.baseColor.a);
    out_normal = vec4(n * 0.5 + 0.5, 1.0);
    out_surface = vec4(metallic, roughness, ao, matParams.surface.w);
    // The emissive RADIANCE, already multiplied, so the lighting pass only has
    // to add it. `emission` above is misc.rgb * strength, optionally modulated
    // by the emissive map — see PBRMaterialParams::pack() for how an unset
    // emission colour resolves.
    out_emissive = vec4(emission, 1.0);
}
