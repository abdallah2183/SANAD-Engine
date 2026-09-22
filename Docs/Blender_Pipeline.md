# Blender → NOVAForge Character Pipeline (glTF-first)

One-click path from Blender to a skinned, animated character in the engine.
No engine source changes, no FBX, no plugins beyond the shipped add-on.

> **العربية:** ملخّص بالعربية في نهاية هذا الملف.
> المسار: تثبيت الإضافة من `Templates/Blender/nf_gltf_export.py`، ثم
> File > Export > NOVAForge glTF، ثم الاستيراد بـ `NFModelImporter`.

## 1. Install the export preset (once)

1. Blender → `Edit > Preferences > Add-ons > Install…`
2. Pick `Templates/Blender/nf_gltf_export.py` from the repo.
3. Enable **Import-Export: NOVAForge glTF Export**.

This adds `File > Export > NOVAForge glTF (.glb)` with settings fixed to
exactly what the engine importer consumes (see the add-on header for the
setting-by-setting rationale).

## 2. Author the character

- **Mesh:** anything; modifiers are applied at export. Keep **≤ 4 bone
  influences per vertex** (Blender default; the engine binding is 4).
- **Skeleton:** one armature, deform bones only are exported
  (`export_def_bones`). Export from the **rest pose** — the armature's
  current pose becomes the skeleton rest pose.
- **Clips:** every non-muted action on the armature becomes one glTF
  animation. Push each action to the NLA strip list (or keep them on the
  armature) so the exporter sees them. Two clips minimum for the acceptance
  check (e.g. `Idle`, `Wave`).
- **Material:** Principled BSDF — base color / metallic / roughness factors
  are imported (textures are a later phase; slots reference material order).

## 3. Export and import

```bash
# Export: File > Export > NOVAForge glTF (.glb)  →  Hero.glb

# Import (CLI): mesh to cooked format + full report
Tools/ModelImporter/build binary:
  NFModelImporter --input Hero.glb --output Content/Meshes/Hero.nfmesh
  NFModelImporter --input Hero.glb --info        # skins, joints, clips
```

`--info` prints, explicitly:

- mesh count, per-mesh material slot, bounds;
- skins: name, joint count, root joint;
- animations: name, duration, channel count;
- anything the importer could not use (non-triangle primitives, CUBICSPLINE
  channels, morph targets) as explicit skip counts — **never a silent
  fallback**. A mesh without a skin binding is reported as `skin_index == -1`
  (static), not substituted with a fake rig.

Programmatic import (`C++`):

```cpp
nf::assets::GltfImportResult r = nf::assets::import_gltf_file("Hero.glb");
nf::animation::Skeleton skel;
nf::assets::make_skeleton(r, /*skin_index=*/0, skel, err);

auto bone_of_node = nf::assets::joint_node_to_bone(r, 0);
for (const auto& anim : r.animations) {
    nf::animation::AnimationClip clip;
    nf::assets::make_clip(r, anim, skel, bone_of_node, clip, err);
    // clip.sample(t, skel, pose) — plays through the runtime animation API
}
```

## 4. Verified round trip

Three test files pin this pipeline. All run in the normal sweep; a skip is never
a pass, and all of them name their cases in the run output.

**`Tests/AssetTests/test_gltf_character_import.cpp`** (6 cases, `AssetTests`) —
synthesizes, fully in memory, the exact document the add-on emits (mesh +
JOINTS_0 u16 / WEIGHTS_0 normalized u16, skin with inverse bind matrices, two
sampled action clips, Principled material) and asserts:

- skeleton bones, parents, rest transforms match the rig;
- per-vertex joint/weight bindings are read and normalized;
- both clips import with correct durations and channels;
- `AnimationClip::sample` produces the animated pose at key times through the
  runtime API (the "plays" half, at animation-data level);
- a static document reports zero skins/animations explicitly (no
  substitution), and unsupported channels are counted, not dropped;
- a malformed JOINTS_0-without-WEIGHTS_0 primitive is rejected and counted, and
  a channel aimed at a non-joint node fails loudly with the node named.

**`Tests/ToolTests/test_model_importer_cli.cpp`** (5 cases, `ToolTests`) —
writes a real `.glb` to disk in a scratch directory (the container
`export_format="GLB"` produces), then runs the real `NFModelImporter.exe` as a
subprocess and asserts what the tool prints and writes:

- the `--info` report names the skin, the joint, both clips and the material;
- the skip ledger is present and exact (1 CUBICSPLINE channel counted, 0
  primitives skipped, 0 skin bindings rejected);
- `--info` writes **nothing** to disk;
- cooking produces an `.nfmesh` that loads back with the right vertex, index
  and submesh counts;
- a prop with no rig is reported as `static mesh (skin_index = -1)`;
- an incomplete invocation exits 2 with the usage text, and a missing file
  exits 1 with a reason.

**`Tests/ToolTests/test_blender_preset.cpp`** (2 cases, `ToolTests`) — the
preset itself. Blender cannot run here, so these pin the part that breaks
silently: the option names the add-on passes to `bpy.ops.export_scene.gltf`.
An unknown keyword makes the operator raise `TypeError`, so the export never
runs and the developer sees a one-click path that does nothing. The three
commonly-written-wrong names are asserted absent and their real spellings
asserted present, and the version-drift warning is pinned so it cannot be
removed.

Last verified on this tree: `AssetTests` **57 passed / 0 failed / 0 skipped**,
`ToolTests` **27 passed / 0 failed / 0 skipped**. Whole-tree sweep (25 suites):
**1435 passed / 0 failed / 2 skipped** (both skips env-gated: `NF_BENCH_1M` in
`ECSTests`, and the live-pad `InputTests` case).

Blender itself is not a test dependency — the fixtures are the documents the
add-on emits (same attributes, component types, and sampling style). The
Blender-side half of the acceptance check is the manual export step in §1–§3.

## 5. Current gap (honest state)

Imported skin/clip data reaches the runtime animation API (skeleton, clips,
sampling). Not yet wired, by ownership and scope:

- **Skinned rendering:** `.nfmesh` v1 has no per-vertex joint/weight
  channels and the renderer has no GPU skinning path — the cooked mesh
  renders unskinned. Skinning belongs to the animation/renderer track (G4 +
  render core), requested via `COORDINATION.md`.
- **Runtime playback hookup:** the scene format's `Animation:` lines use
  procedural clips; binding imported clips to a scene entity is
  `Engine/Runtime/**` (runtime track). The data is import-complete; the
  last mile is a coordinator request, not a format gap.

## 6. Troubleshooting

- **"skin has no joints" / missing clips:** the armature was not selected at
  export, or actions are muted. Select mesh + armature, unmute the strips.
- **Character explodes at import-time sampling:** exported from a pose other
  than rest. Re-export with the armature in rest pose.
- **More than 4 influences:** the exporter caps at 4 (`export_all_influences`
  off); weights beyond 4 are Blender's job to limit (Vertex Weight Edit).
- **A warning naming an unsupported option** (e.g. "this Blender build has no
  `export_force_sampling`"): Blender renames exporter options between releases.
  The add-on drops the options the installed build does not declare and **names
  them in the warning** instead of failing the export, so the export still runs —
  but read the warning, because a dropped option means that setting did not
  apply. The names shipped here are checked against Blender's current
  `bpy.ops.export_scene.gltf` API; the three most commonly written wrong are
  `use_selection` (not `export_selected_objects`), `export_force_sampling` (not
  `export_sampling`) and `export_def_bones` (not `export_def_bones_only`).

---

## العربية (ملخّص)

**خط أنابيب الشخصية من بلندر إلى المحرك — بخطوة واحدة:**

1. ثبّت الإضافة: `Edit > Preferences > Add-ons > Install…` واختر
   `Templates/Blender/nf_gltf_export.py` من المستودع.
2. من بلندر: `File > Export > NOVAForge glTF (.glb)` — الإعدادات مثبّتة
   مسبقًا: +Y، تطبيق المعدّلات، عظام الإلحاق فقط، حتى 4 أوزان لكل رأس،
   وكل أكشن غير مكتوم يصير مقطعًا (clip).
3. الاستيراد: `NFModelImporter --input Hero.glb --output Hero.nfmesh`
   و`--info` لطباعة الهياكل والمقاطع صراحةً — لا استبدال صامت لأي صيغة.
4. في C++: `import_gltf_file` ثم `make_skeleton` ثم `make_clip` لكل مقطع،
   و`clip.sample()` يشغّل الحركة عبر واجهة الأنيميشن في المحرك.
5. **مُتحقَّق منه بثلاثة ملفات اختبار:**
   `Tests/AssetTests/test_gltf_character_import.cpp` (٦ حالات: الهيكل، الأوزان،
   المقطعان، الرفض الصريح للوثيقة الساكنة)،
   `Tests/ToolTests/test_model_importer_cli.cpp` (٥ حالات: يشغّل أداة الاستيراد
   الحقيقية على ملف `.glb` حقيقي ويتحقّق من تقرير `--info` ومن الخَبز إلى
   `.nfmesh`)، و`Tests/ToolTests/test_blender_preset.cpp` (حالتان: أسماء
   خيارات المُصدِّر التي يقبلها بلندر فعلًا، وتحذير الإصدارات).
   آخر قياس: `AssetTests` ‏57/57، `ToolTests` ‏27/27، والمجموع الكامل
   ‏1435/1435 مع تخطّيين مقيَّدين بمتغيّر بيئة.
6. **الفجوة الحالية:** الرندر المُجمَّع (GPU skinning) وربط المقاطع
   بمكوّن المشهد في Runtime — خارج نطاق مسار الاستيراد، مرفوعة كطلبات
   تنسيق، والبيانات المستوردة كاملة على جانب الاستيراد.
