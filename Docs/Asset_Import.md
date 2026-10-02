# NOVAForge Asset Import — the mesh read path

Everything the engine can **write**, it can now **read back**. That symmetry is
the whole point of this document: before `nf::assets::MeshImport` existed, the
engine exported six mesh formats (`MeshExport`) and imported exactly one
(`GltfImport`, `.gltf`/`.glb`). Exporting a mesh to OBJ produced a file the
engine could not open again, and the editor's Import dialog offered
`*.gltf;*.glb` while `ImportQueue::submit()` answered *"Unsupported extension"*
for both.

> **العربية:** ملخّص بالعربية في نهاية هذا الملف.
> المسار: المحرر ← `File > Import`، أو سطر الأوامر `NFModelImporter`، أو واجهة
> `nf::assets::import_mesh_file`. كلها تستخدم قارئًا واحدًا، فتستورد نفس الصيغ
> بالضبط.

## 1. Formats

| Extension | Reader | Carries | Does **not** carry |
|---|---|---|---|
| `.nfmesh` | `MeshAsset` (the engine's own container) | everything — it *is* the cooked mesh | — |
| `.gltf` | cgltf, sibling `.bin` resolved beside the file | meshes, nodes, materials, skins, animations | textures (later phase) |
| `.glb` | cgltf, GLB container | same as `.gltf` | same as `.gltf` |
| `.obj` | `MeshImport.cpp` | positions, normals, one UV channel, `o`/`g` groups, `usemtl` slots | uv1, tangents, multiple UV channels |
| `.stl` | `MeshImport.cpp` (binary **and** ASCII) | triangles, one facet normal each | indices, UVs, vertex normals |
| `.ply` | `MeshImport.cpp` (ASCII + binary, both endiannesses) | positions, normals, UVs, faces | uv1, tangents |

Losses are never silent. Every reader fills `MeshImportResult::warnings` with
what the format itself could not express — for example *"STL has no index
buffer, no UVs and no vertex normals; geometry is flat-shaded and UVs default to
zero"* — and the editor shows those warnings on the import row.

**FBX is not supported, deliberately.** FBX is a proprietary binary format
requiring a large vendored parser. The project's standing decision (see
`.workbuddy-ai/agents/model-g2-assets.md`) is the **glTF-first route**: export
from Blender with the shipped add-on
(`Templates/Blender/nf_gltf_export.py`, one click under
`File > Export > NOVAForge glTF`), which carries mesh, skeleton, clips and
materials. See `Docs/Blender_Pipeline.md`.

### Materials and textures

Geometry on its own arrives grey, which is not an import a developer can use. So
the reader carries materials and textures out with the mesh:

- **glTF** — base colour, metallic, roughness and emissive factors come from the
  material; the base-colour map is resolved to an entry in `images`. Images are
  extracted from a GLB's BIN chunk, from a base64 `bufferView`, from a `data:`
  URI, or from an external file beside the `.gltf`.
- **OBJ** — an OBJ carries *no* material values at all; `usemtl` only names one.
  The reader therefore resolves the `.mtl` its `mtllib` line points at, so `Kd`,
  `d`/`Tr` and `map_Kd` survive. A submesh's `material_slot` becomes the index of
  a real material in first-use order — which also reproduces MeshExport's own
  `slot0`, `slot1`, … numbering on a round trip.
- **Images keep their original encoded bytes** (PNG/JPEG exactly as the file
  stored them). Decoding is the renderer's job, and re-encoding here would lose
  data for nothing. The extension is sniffed from the bytes, not trusted from a
  filename that may lie.

The engine's material block holds **one** texture and has no specular, clearcoat,
sheen, transmission or normal map. Everything the source declared that has
nowhere to go is **named per material** in `GltfMaterialInfo::dropped` and shown
in the import row — a Blinn-Phong exponent (`Ns`) is never quietly turned into a
roughness, and a specular colour (`Ks`) is never folded into metallic. That would
hand the renderer a material the artist did not author.

What the editor writes for a model import:

```
content://Meshes/<stem>.nfmesh              the cooked geometry
content://Textures/<stem>_<image>.<ext>     registered texture assets (+ cache copy)
content://Materials/<stem>_<material>.nfmat the material, with `albedo:` already
                                            pointing at the texture above
```

`.nfmat` is the only route a material has into the engine, because `.nfmesh`
carries geometry and nothing else. The submesh `material_slot` indexes the
material list in the same order, so a consumer can pair them up.

The `NFModelImporter` CLI cooks geometry only (its output is a `.nfmesh`), and
says so explicitly when it is leaving materials and images behind. Use the
editor's `File > Import` when you want them written as assets.

## 2. Three ways in

**Editor.** `File > Import` (Ctrl+I) → Browse → pick any file the filter
offers. Models are cooked to `.nfmesh` and registered immediately, so they are
usable from the asset browser, drag-and-drop and the albedo picker without a
second step.

**CLI.**

```bash
NFModelImporter --input props/Crate.obj --output Content/Meshes/Crate.nfmesh
NFModelImporter --input Hero.glb --info          # report only, writes nothing
NFModelImporter --input Hero.glb --character     # strict validation, writes nothing
```

**API.**

```cpp
nf::assets::MeshImportResult r = nf::assets::import_mesh_file("props/Crate.obj");
if (!r.ok) { /* r.error says why */ }
for (const auto& mesh : r.meshes) { /* MeshAsset, ready to cook */ }
```

All three call the same function, so a format one of them accepts is a format
all of them accept.

## 3. Rules the importers obey

**Extension first, content as fallback.** A known extension wins; content
sniffing (`sniff_mesh_format`) is used only when the extension is missing or
unknown. When the extension names one format family and the bytes plainly
belong to another, the import **fails** with `extension/content mismatch`
instead of guessing. glTF and GLB share one family, because the same parser
reads both and owns its own stricter version of that check.

**No silent substitution.** A malformed face is counted in `faces_skipped`, a
non-finite vertex in `vertices_skipped`, an unsupported glTF primitive in
`primitives_skipped`, an unsupported animation channel in
`anim_channels_skipped`. Nothing is quietly repaired, and nothing is quietly
dropped.

**Multi-mesh sources keep every mesh.** A glTF with three meshes, or an OBJ with
three `o`/`g` groups, imports as three assets:

```
content://Meshes/<stem>.nfmesh      (mesh 0)
content://Meshes/<stem>_1.nfmesh    (mesh 1)
content://Meshes/<stem>_2.nfmesh    (mesh 2)
```

The uncooked source file is **not** copied into the project. The cooked mesh is
the asset; a second, editable copy is an invitation to edit the wrong one.

**Overwrite is all-or-nothing.** A multi-mesh import checks every destination
before writing any of them, so a failure half-way cannot leave a partially
updated asset behind.

## 4. Verified round trip

`Tests/AssetTests/test_mesh_import.cpp` proves the symmetry by round trip rather
than by fixture: it exports a mesh with the engine's real writer, imports it
back with the engine's real reader, and compares. A fixture would only prove the
parser agrees with the test author.

A 24-vertex / 12-triangle cube, exported and re-imported:

| Format | Vertices back | Triangles back | Materials back | Notes |
|---|---|---|---|---|
| `.nfmesh` | 24 | 12 | — | lossless by construction |
| `.obj` | 24 | 12 | `slot0`, `Kd 0.8` from the `.mtl` | index-triple dedupe recovers the exact vertex set |
| `.stl` | 24 | 12 | — | re-shared where position **and** normal agree |
| `.ply` | 24 | 12 | — | exact, including UVs and normals |
| `.glb` | 24 | 12 | `default` | goes through the glTF reader, reports `skin_index = -1` |

The OBJ row is the one that needed a `.mtl` reader: without it the cube came back
as untextured grey geometry with an arbitrary slot number.

Plus hand-written fixtures for what a round trip cannot reach: negative OBJ
indices, OBJ n-gons, `g` groups, `v//vn` faces, ASCII STL, ASCII PLY with an
extra per-vertex property, binary-little-endian PLY, an OBJ + `.mtl` pair with a
base-colour map, and glTF textures embedded both as a `bufferView` and as a
`data:` URI. A rejected face is also checked to leave **no orphan vertices**
behind: a face that fails on its third vertex must not inflate the asset's
vertex array.

## 5. Troubleshooting

- **"extension/content mismatch"** — the file is not what its name says. Rename
  it to the format it actually is (check the first four bytes).
- **"cannot determine the mesh format"** — unknown extension *and* no
  recognisable signature. OBJ is the usual case: it has no magic bytes, so it
  must keep its `.obj` extension.
- **"PLY has no usable faces (a point cloud is not a triangle mesh)"** — the PLY
  has vertices but no `face` element. The engine needs triangles.
- **"OBJ contains no usable faces"** — vertices but no `f` lines, or every `f`
  line was malformed. The usual cause is an index pointing past the end of a
  pool: `f 2//2` in a file with a single `vn` is a broken file, and the reader
  says so instead of inventing a normal. Check `faces_skipped` in the report.
- **Mesh imports but the texture is wrong** — read the import row's `!` warning.
  STL has no UVs at all, and an OBJ that carried no `vn` gets computed smooth
  normals.
- **A material came in without its specular / bump map** — expected. The engine's
  material block has room for one texture; the report names each dropped slot
  under the material it belongs to.
- **An imported texture will not load** — its warning says the container was not
  recognised and it was written as `.bin`. The engine decodes PNG, JPEG, BMP,
  TGA and GIF; anything else is carried through but cannot be used.

---

## العربية (ملخّص)

**المشكلة:** المحرك كان يكتب ست صيغ للنماذج (`.nfmesh`، `.obj`، `.stl`، `.ply`،
`.gltf`، `.glb`) لكنه كان يقرأ صيغة واحدة فقط (`.gltf`/`.glb`). أي أن تصدير
نموذج إلى OBJ ينتج ملفًا لا يستطيع المحرك فتحه. وكانت نافذة الاستيراد في المحرر
تعرض `*.gltf;*.glb` بينما يرفضها صندوق الاستيراد برسالة "Unsupported extension".

**الحل:** قارئ موحّد `nf::assets::MeshImport` يقرأ الصيغ الست نفسها، مع قواعد
صريحة:

- **لا استبدال صامت للصيغة:** الامتداد أولًا، وتفتيش المحتوى احتياطًا؛ وإذا
  تعارض الامتداد مع البايتات يفشل الاستيراد برسالة `extension/content mismatch`
  بدلًا من التخمين.
- **لا فقدان صامت:** كل ما لا تستطيع الصيغة حمله يُسمّى في `warnings`، وكل ما
  أُسقط يُعَدّ (`faces_skipped`، `vertices_skipped`، `primitives_skipped`،
  `anim_channels_skipped`، `images_skipped`).
- **الموادّ والخامات تُستورد مع الشبكة**، لأن الهندسة وحدها تصل رمادية:
  - **glTF:** عوامل اللون الأساسي والمعدنية والخشونة والإشعاع، مع خامة اللون
    الأساسي — تُستخرج من مقطع BIN في GLB، أو من `bufferView` بترميز base64، أو
    من `data:` URI، أو من ملف خارجي بجانب `.gltf`.
  - **OBJ:** الملف نفسه لا يحمل أي قيم موادّ، بل أسماء فقط عبر `usemtl`؛ لذلك
    يقرأ القارئ ملف `.mtl` المشار إليه في `mtllib` فتبقى `Kd` و`d`/`Tr` و
    `map_Kd`. ورقم المادة (`material_slot`) يصبح فهرس مادة حقيقية بترتيب أول
    استخدام — وهو ما يعيد ترقيم `slot0`/`slot1` الذي يكتبه المصدّر نفسه.
  - **الصور تبقى ببايتاتها الأصلية** (PNG/JPEG كما خزّنها الملف)، والامتداد
    يُستنبط من البايتات لا من الاسم.
- **ما لا يسعه المحرك يُسمّى ولا يُقدَّر:** كتلة المادة في المحرك تحمل خامة واحدة
  ولا مكان فيها للّمعان أو الطلاء أو خريطة النورمال. لذلك يُسمّى كل حقل مُسقَط
  لكل مادة في `GltfMaterialInfo::dropped` ويظهر في صف الاستيراد — ولا يُحوَّل
  أُس Blinn-Phong (`Ns`) صامتًا إلى خشونة، ولا يُدمج لون اللمعان (`Ks`) في
  المعدنية.
- **المحرر يكتب لكل نموذج:** الشبكات المطبوخة، والخامات كأصول مسجّلة في
  `content://Textures`، وملف `.nfmat` لكل مادة في `content://Materials` مع
  `albedo:` يشير إلى الخامة. و`.nfmat` هو الطريق الوحيد لدخول مادة إلى المحرك،
  لأن `.nfmesh` يحمل هندسة فقط. أما أداة `NFModelImporter` فتطبوخ هندسة فقط،
  وتقول ذلك صراحةً عندما تترك موادّ أو صورًا خلفها.
- **نماذج متعددة الشبكات:** تُستورد كلها كأصول مستقلة
  (`<stem>.nfmesh`، `<stem>_1.nfmesh`، ...) بدل الاحتفاظ بواحدة وإسقاط الباقي.
- **لا نسخة من الملف الأصلي:** الشبكة المطبوخة هي الأصل.

**المداخل الثلاثة** (المحرر، `NFModelImporter`، الواجهة البرمجية) تستدعي الدالة
نفسها، فلا يمكن أن تختلف الصيغ المقبولة بينها.

**FBX غير مدعوم بقرار صريح**، والمسار المعتمد هو glTF عبر إضافة Blender
المرافقة (`Templates/Blender/nf_gltf_export.py`) — انظر `Docs/Blender_Pipeline.md`.
