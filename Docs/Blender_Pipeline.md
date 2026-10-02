# Blender → NOVAForge Character Pipeline (glTF-first)

Blender export plus strict source validation for a skinned, animated character.
The current engine can read the source data and sample it through the animation
API, but it does **not** yet cook or render a complete character. No FBX and no
plugins beyond the shipped add-on are required.

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
- **Clips:** the active action plus actions on non-muted NLA tracks are
  exported in `ACTIONS` mode. Make at least two actions visible this way (for
  example `Idle` and `Wave`); an unrelated action merely present in the file
  but not active/stashed is not exported.
- **Material:** Principled BSDF — base color / metallic / roughness factors
  are imported (textures are a later phase; slots reference material order).

## 3. Export and validate

```bash
# Export: File > Export > NOVAForge glTF (.glb)  →  Hero.glb

# Strict character source validation. This writes no file.
.\build\DebugNinja\bin\NFModelImporter.exe --input Hero.glb --character --info

# Report-only for inspection; also writes no file.
.\build\DebugNinja\bin\NFModelImporter.exe --input Hero.glb --info
```

`--character` requires all of the following and prints the full import ledger
before reporting success or failure:

- at least one triangle mesh with a complete valid bound skin;
- at least two nonempty animation clips that convert against that skin;
- material data;
- no skipped primitives, rejected skin bindings, or unsupported animation
  channels.

The strict mode is deliberately **validation-only** in the current version.
`.nfmesh` v1 is a geometry-only `MeshAsset`: it has no joint/weight channels,
skeleton, inverse binds, clips, node hierarchy, or material definitions. The
tool therefore never writes a `.nfmesh` for a character and never calls that
geometry a character cook.

The existing output mode remains available for static props only:

```bash
NFModelImporter --input Prop.glb --output Content/Meshes/Prop.nfmesh
```

It refuses any source containing skin or animation data. A successful static
output says explicitly that it is geometry-only.

Every successful import prints:

- mesh count, per-mesh material slot, bounds;
- skins: name, joint count, root joint;
- animations: name, duration, channel count;
- rejected skin bindings and unsupported STEP/CUBICSPLINE/morph channels.

A mesh without a skin is reported as `skin_index == -1` (static), not
substituted with a fake rig.

Programmatic import below is a diagnostic/embedding API, not a replacement for
the missing cooked-character and runtime route:

```cpp
nf::assets::GltfImportResult r = nf::assets::import_gltf_file("Hero.glb");
auto validation = nf::assets::validate_character_import(r);
if (!validation.ok) return false;

nf::animation::Skeleton skel;
nf::assets::make_skeleton(r, static_cast<nf::usize>(validation.skin_index),
                          skel, err);
```

## 4. Verified round trip

Three test files pin this pipeline. All run in the normal sweep; a skip is never
a pass, and all of them name their cases in the run output.

**`Tests/AssetTests/test_gltf_character_import.cpp`** (11 cases, `AssetTests`) —
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

**`Tests/ToolTests/test_model_importer_cli.cpp`** (9 cases, `ToolTests`) —
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

**`Tests/ToolTests/test_blender_preset.cpp`** (3 cases, `ToolTests`) — the
preset itself. Blender cannot run here, so these pin the part that breaks
silently: the option names the add-on passes to `bpy.ops.export_scene.gltf`.
An unknown keyword makes the operator raise `TypeError`, so the export never
runs and the developer sees a one-click path that does nothing. The three
commonly-written-wrong names are asserted absent and their real spellings
asserted present, and the version-drift warning is pinned so it cannot be
removed.

Last verified on this tree: `AssetTests` **78 passed / 0 failed / 0 skipped**,
`ToolTests` **47 passed / 0 failed / 0 skipped**. Whole-tree sweep (26 suites):
**1589 passed / 0 failed / 2 skipped** (the skips are the env-gated
`NF_BENCH_1M` case in `ECSTests` and the live-pad `InputTests` case).

Blender itself is not a test dependency — the fixtures are the documents the
add-on emits (same attributes, component types, and sampling style). The
Blender-side half of the acceptance check is the manual export step in §1–§3.

## 5. Current gap (honest state)

Imported skin/clip data reaches the runtime animation API (skeleton, clips,
sampling). It is not yet a cooked or playable character:

- **Cooked character target:** `.nfmesh` v1 is geometry-only. A versioned
  character container must preserve geometry, skin bindings, skeleton/inverse
  binds, clips, node/root transform, and material definitions. Until that
  exists, `--character` validates and writes nothing.
- **Skinned rendering:** there are no joint/weight vertex inputs or per-object
  joint-palette bindings in the renderer, so imported non-root bone motion is
  not visible. This is G4 + render-core work.
- **Runtime playback hookup:** `.nfscene` drives procedural clips only and
  cannot reference an imported character/clip. A data-driven scene reference
  and save/load path are required in the runtime track.

## 6. Troubleshooting

- **"skin has no joints" / missing clips:** the armature was not selected at
  export, or actions are muted. Select mesh + armature, unmute the strips.
- **Character explodes at import-time sampling:** exported from a pose other
  than rest. Re-export with the armature in rest pose.
- **More than 4 influences:** the exporter caps at 4 (`export_all_influences`
  off); weights beyond 4 are Blender's job to limit (Vertex Weight Edit).
- **Critical exporter option missing:** the add-on cancels the export and
  names the option. It never continues with a weaker character contract.
- **Optional exporter option missing:** the add-on names it in a warning after
  a successful export. The shipped critical set includes selection, Y-up,
  animation sampling, skins, deform bones, and four-influence limits.
- **Exporter says finished but the file is missing/empty:** the add-on cancels
  and reports the exact output path instead of claiming success.

---

## العربية (ملخّص)

**خط أنابيل استيراد شخصية من بلندر والتحقق منها:**

1. ثبّت الإضافة: `Edit > Preferences > Add-ons > Install…` واختر
   `Templates/Blender/nf_gltf_export.py` من المستودع.
2. من بلندر: `File > Export > NOVAForge glTF (.glb)`. الإضافة ترفض التصدير
   إذا لم يوجد mesh مشوَّه، وarmature محدَّد، ومادة Principled، وفعلان
   على الأقل مرتبطان بالـarmature؛ كما تتحقق من خيارات التصدير الأساسية
   ومن نتيجة المشغّل ومن وجود ملف nonempty.
3. التحقق الصارم:
   `NFModelImporter --input Hero.glb --character --info`.
   هذا يطبع دفتر الاستيراد كاملًا ويتطلب skin صالحًا ومقطعين على الأقل
   ومادة، لكنه **لا يكتب ملفات** في النسخة الحالية.
4. صيغة `.nfmesh` الحالية هندسة فقط: لا تحمل skin أو skeleton أو clips أو
   تعريفات المواد. لذلك ترفض الأداة تحويل شخصية إلى mesh ثابت وتصرّح
   بذلك؛ لا يوجد بعد خَبز شخصية كامل.
5. لخصائص ثابتة بلا skin أو animation، يبقى المسار متاحًا بوضوح:
   `NFModelImporter --input Prop.glb --output Prop.nfmesh`.
6. **الفجوة الحالية:** حاوية شخصية cooked، الرندر المُجمَّع (GPU skinning)،
   وربط المقاطع المستوردة بمشهد Runtime. البيانات متاحة عبر واجهة
   `AnimationClip` في C++، لكن هذه ليست بديلًا عن مسار playable بلا C++.
