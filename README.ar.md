<div align="center">

<img src="Docs/images/banner.png" alt="محرك سَنَد — محرك ألعاب عربي ثلاثي الأبعاد" width="100%">

# محرك سَنَد · SANAD Engine

<div dir="rtl">

**محرك ألعاب ثلاثي الأبعاد مكتوب بـ C++23 وVulkan — ومحرر يتكلم العربية من أول يوم.**

<a href="https://github.com/abdallah2183/SANAD-Engine/releases/latest"><img alt="أحدث إصدار" src="https://img.shields.io/github/v/release/abdallah2183/SANAD-Engine?style=flat-square&label=release"></a>
<a href="LICENSE"><img alt="الترخيص Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square"></a>
<a href="#"><img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?style=flat-square&logo=c%2B%2B"></a>
<a href="#"><img alt="Vulkan 1.2+" src="https://img.shields.io/badge/Vulkan-1.2%2B-red?style=flat-square&logo=vulkan"></a>
<a href="#"><img alt="Windows x64" src="https://img.shields.io/badge/platform-Windows%20x64-0078D4?style=flat-square"></a>
<a href="#"><img alt="26 حزمة اختبار" src="https://img.shields.io/badge/tests-26%20suites-brightgreen?style=flat-square"></a>
<a href="AGENTS.md"><img alt="سير عمل جاهز للوكلاء" src="https://img.shields.io/badge/AI--ready-agents-6f42c1?style=flat-square"></a>

<br>

[**جديد 0.5.0**](RELEASE_NOTES_0.5.0.md) ·
[**تنزيل آخر حزمة (0.5.0)**](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.5.0) ·
[**شاهد الفيديو (43 ثانية)**](Docs/media/demo.mp4) ·
[**التوثيق**](Docs/) ·
[**English**](README.md)

<br>

<img src="Docs/media/hero.gif" alt="محرر سَنَد يبني مشهدًا ثم ديمو المركبات" width="860">

</div>

</div>

---

<div dir="rtl">

> [!IMPORTANT]
> **0.5.0 إصدار عام، وليس بيتا.** يُنشر كـ**نسخة محمولة لويندوز x64** مبنية من
> هذه الشجرة، ومعها بصمة SHA-256. الثنائيات موقّعة بشهادة تطوير ذاتية
> (`SANAD Dev Build`)، لا بشهادة عامة موثوقة، فقد يظهر "ناشر غير معروف" على جهاز
> لا يثق بالشهادة. لا تعطّل Smart App Control أو WDAC لتشغيلها — راجع تحذير
> التوقيع/السياسة. يلزم تعريف GPU يدعم Vulkan.

## 🌟 لماذا سَنَد؟

- **عربي أولًا، لا لاحقًا.** المحرر يشكّل النصوص العربية بشكل صحيح (أشكال
  سياقية، لام-ألف، ثنائي الاتجاه) بخط Amiri، مع تعريب كامل للواجهة: المشغّل،
  المحرر، دليل البداية، وحوار الحفظ.
- **محرر حقيقي.** نوافذ قابلة للإرساء، شجرة مشهد، مفتش خصائص، أدوات تحريك/
  تدوير/تحجيم، تراجع/إعادة، متصفح أصول، وإعادة تحميل فوري، ودليل **Start Here**
  يوصل المستخدم الجديد إلى مشهد قابل للتشغيل.
- **مبني كمحرك.** C++23، واجهة Vulkan 1.2+، ECS موجّه بالبيانات، وRenderGraph
  على شكل DAG، مع فرض طبقات صارم.
- **جاهز للذكاء الاصطناعي، لا مجرد شعار.** الريبو وCLI وكل مشروع جديد يحملون
  نفس العقد القابل للتنفيذ، فيبني الوكيل `.nfproj` أصليًا بدل الانحراف إلى ويب.
- **سير عمل كامل.** أنشئ مشروعًا، أضف مجسمات، اضغط **Play**، احفظ بأمان مع نسخة
  استرداد، ثم اصنع لاعبًا مستقلًا.
- **مفتوح المصدر.** Apache-2.0.

## 🚀 البدء السريع

### 1. نزّل النسخة

من صفحة [إصدار 0.5.0](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.5.0):

| الملف | ماذا تفعل |
|:---|:---|
| [`SANAD-0.5.0-Windows-x64-Portable.zip`](https://github.com/abdallah2183/SANAD-Engine/releases/download/v0.5.0/SANAD-0.5.0-Windows-x64-Portable.zip) | فك الضغط في أي مكان → شغّل `SANADEditor.exe` |
| [`SHA256SUMS.txt`](https://github.com/abdallah2183/SANAD-Engine/releases/download/v0.5.0/SHA256SUMS.txt) | بصمة SHA-256 للحزمة المحمولة |

> [!NOTE]
> **لا يوجد مثبّت في هذا الإصدار بعد.** الحزمة تحمل توقيع تطوير ذاتيًا، فقد تظهر
> رسالة "ناشر غير معروف" على جهاز لا يثق بالشهادة. النسخة لويندوز 10/11 x64 فقط.

### 2. أنشئ مشروعًا في 30 ثانية

1. `SANADEditor.exe` → **إنشاء مشروع جديد**
2. اختر **Third-Person**، اكتب الاسم، اضغط **إنشاء**
3. يفتح المشهد مع دليل البداية → اضغط **Play** (أو `Ctrl+P`)

### 3. أو من سطر الأوامر

```bat
nf.exe new MyGame --name MyGame --template ThirdPerson
nf.exe build --project MyGame\MyGame.nfproj
cd MyGame\dist && NFPlayer.exe
```

## 🤖 استخدام وكيل ذكاء اصطناعي

سَنَد مصمّم ليقوده وكيل برمجي، لا ليُستبدل بلعبة ويب. إذا فتحت Claude Code أو
Codex أو Cursor أو أي وكيل على هذا الريبو:

- سيقرأ [`AGENTS.md`](AGENTS.md) تلقائيًا؛ و`CLAUDE.md` و`.cursor/rules/`
  و`llms.txt` مداخل للأدوات الشائعة.
- `nf guide` يطبع نفس القاعدة وسير العمل في الطرفية.
- كل مشروع ينشئه `nf new` يحصل على `AGENTS.md` خاص به، فالوكيل المفتوح داخل
  **مجلد اللعبة** يعرف أيضًا ماذا يفعل.
- الوصفة القابلة للتنفيذ في [`Docs/AI_Game_Guide.md`](Docs/AI_Game_Guide.md):
  أنشئ `.nfproj`، عدّل `.nfscene` وLua، ثم `nf run`.

**القاعدة الصارمة:** لعبة سَنَد هي `.nfproj` أصلية تُبنى بـ`nf` — وليست
`index.html` أو خادم ويب أو مشروع Unity/Godot.

## 🎬 شاهدها تعمل

[![شاهد الفيديو](Docs/images/demo_poster.png)](Docs/media/demo.mp4)

*43 ثانية: المشغّل → المحرر → بناء المشهد → Play → حماية الحفظ → الحزمة المحمولة.*

| المشغّل العربي | المحرر العربي |
|:---:|:---:|
| <img src="Docs/images/shot_launcher_ar.png" alt="مشغّل المشاريع العربي" width="420"> | <img src="Docs/images/shot_editor_ar.png" alt="المحرر العربي RTL" width="420"> |

| ديمو المركبات | حماية الشغل |
|:---:|:---:|
| <img src="Docs/images/shot_vehicle.png" alt="ديمو المركبات" width="420"> | <img src="Docs/images/shot_save_guard.png" alt="حوار الحفظ قبل الإغلاق" width="420"> |

## 🎮 لعبة تجريبية: TreeWorld

لعبة كاملة مبنية ببيانات سَنَد فقط — مستكشف منخفض التفاصيل يمشي في غابة ضبابية
ويجمع خمس يراعات. المصدر والمشهد وLua والمواد والصور الإجرائية في
[`Games/TreeWorld`](Games/TreeWorld)؛ الأسلوب low-poly لأنه لا توجد حزمة أصول
غابة ممسوحة ضوئيًا بعد.

<img src="Games/TreeWorld/Screenshot.png" alt="TreeWorld — غابة ضبابية وشخصية بمنظور الشخص الثالث" width="860">

```bat
build\Release\bin\nf.exe run --project Games\TreeWorld\TreeWorld.nfproj
```

> [!NOTE]
> TreeWorld تأتي داخل حزمة **0.5.0** المحمولة تحت `Games/`، وعلى فرع **`main`**
> كمصدر. استخدم `build_nf.bat Release` لتشغيلها من المستودع، أو شغّل النسخة
> المعبّأة نفسها من الحزمة.

## ✨ جديد في 0.5.0

- **FXAA في مكانه الصحيح** — ممرّ FXAA 3.11 على صورة LDR بعد tonemap لا على HDR،
  مع عتبات قابلة للضبط وصورة وسيطة يملكها العارض.
- **الفأرة صارت زرًا** — الزر الأيمن يصوّب (ADS)، والكاميرا تهتز وترتد. إعدادات
  اللاعب (حساسية الفأرة والعصا، مسافة التصويب، حدود التقريب، عكس Y) بيانات لا
  كود، و`nf.shake()` / `nf.recoil()` متاحتان من Lua.
- **ملصقات مسقطة** — ممرّ ملصقات بعد GBuffer مع واصفات لكل ملصق وقصّ وترتيب،
  ومكوّن **Decal** في المحرر وأمر `SetDecalCommand` قابل للتراجع.
- **مقياس رسم داخلي** — `render_scale=0.25..2.0` يفصل حجم المخرج عن حجم الرسم،
  فـ`2.0` يزيد الدقة الداخلية بلا تغيير حجم النافذة.
- **دقة ديناميكية** — `dynamic_resolution=true target_fps=… min_render_scale=…`
  تخفض المقياس وتعيده بحسب زمن الإطار، مع منطقة تردد تمنع التذبذب.
- **ديناميكيات الماستر** — ضاغط ستيريو مرتبط ومحدّد ذروة بنافذة استباق في
  `AudioScene::finalize`، ومفاتيح `audio.dynamics.*` في المشهد.
- **إعدادات اللاعب والإدخال** — ملف إعدادات لكل لاعب، وأزرار الفأرة في
  `InputBindings`، ومفاتيح مشهد تحافظ على توافق المشاريع القديمة.
- **TreeWorld وThirdPerson** يأتيان بـ`fxaa=true` و`ads=1`، فالمسار الجديد
  والتصويب ظاهران من أول تشغيل.
- **متحقَّق منه لا مفترض** — ملاحظات 0.5.0 تسرد النتائج: `RHITests` 320/320،
  `RuntimeTests` 214/214، `EditorTests` 283/283، `UITests` 44/44،
  `ToolTests` 55/55، `AudioTests` 132/132؛ و`StreamingTests` محجوب بسياسة
  Device Guard على هذه الآلة. وقبول TreeWorld بلا نافذة يخرج بـ0 مع
  `Validation errors: 0`.
- **توثيق يطابق البناء** — [ملاحظات الإصدار](RELEASE_NOTES_0.5.0.md) و
  [ملاحظات الديمو](Docs/Demo_Release.md) ودليل بدء سريع.

## 🧱 المزايا

| المجال | ما يقدّمه |
|:---|:---|
| **التصيير** | Vulkan 1.2+، RenderGraph، PBR مؤجل (GGX)، ظلال متتالية، سماء إجرائية، IBL، SSAO، Bloom، اختيار GPU، LOD |
| **المحرر** | إرساء نوافذ ImGui، Outliner، مفتش خصائص، Gizmos، تراجع/إعادة، متصفح أصول، دليل البداية، واجهة عربية |
| **الفيزياء** | محلّل حتمي + Jolt (مركبات، ragdolls، CCD، heightfields)، قماش PBD |
| **السكربتات** | Lua 5.4 معزول (`nf.*`) + C# عبر .NET 10 |
| **الأصول** | استيراد glTF/GLB/OBJ/STL/PLY و`.nfmesh`، طهي الأصول، مواد وصور، إعادة تحميل فوري |
| **الصوت** | صوت مكاني 3D عبر WASAPI؛ WAV/OGG/MP3/FLAC |
| **ثنائي الأبعاد** | `NFScene2D`: كاميرات، Sprites، Tilemaps، فيزياء 2D، A* |
| **الأدوات** | `nf` (new/build/verify/run)، AssetCooker، ModelImporter، Player |

<details>
<summary><b>بوابات الجودة</b></summary>

- بناء MSVC نظيف بـ `/W4 /WX`.
- **26 حزمة اختبار.** حزم 0.5.0 نجحت على ويندوز x64 (Release): `RHITests`
  320/320، `RuntimeTests` 214/214، `EditorTests` 283/283، `UITests` 44/44،
  `ToolTests` 55/55، `AudioTests` 132/132. `StreamingTests` محجوب بسياسة
  Device Guard على هذه الآلة، وليس فشلًا في الاختبار.
- 0 أخطاء Vulkan validation و0 تسريبات RHI في اختبار قبول المحرر.
- اختبار شامل: إنشاء مشروع بـ `nf`، بناؤه، تغليفه، وتشغيل اللاعب من الحزمة.

</details>

## 🔧 البناء من المصدر

**المتطلبات:** ويندوز 10/11 x64، Visual Studio 2022 أو أحدث مع workload
**Desktop development with C++**، CMake 3.25+، Ninja، وVulkan SDK 1.3+
(`glslc` على `PATH`).

```bat
git clone https://github.com/abdallah2183/SANAD-Engine.git
cd SANAD-Engine
build_nf.bat Release
```

ثم:

```bat
build\Release\bin\SANADEditor.exe
build\Release\bin\NFPlayer.exe
```

<details>
<summary><b>CMake يدويًا</b></summary>

```bat
cmake --preset release
cmake --build build/release
```

</details>

> [!NOTE]
> سكربت `packaging/windows/build_installer.ps1` يستطيع إنتاج مثبّت Inno Setup
> على جهاز فيه Inno Setup. إصدار 0.5.0 ينشر الـ zip المحمول فقط.

## 🎮 ابنِ لعبة

```bat
build\Release\bin\nf.exe new MyGame --name MyGame --template ThirdPerson
build\Release\bin\nf.exe build --project MyGame\MyGame.nfproj
cd MyGame\dist
NFPlayer.exe
```

- استيراد المجسمات: **File → Import** أو `NFModelImporter.exe`
  ([الصيغ](Docs/Asset_Import.md)).
- شخصيات Blender: استخدم الإضافة المرفقة ([الدليل](Docs/Blender_Pipeline.md)).
- جديد؟ [من وين أبدأ](Docs/Getting_Started_Ar.md) ·
  [درس 20 دقيقة](Docs/Tutorial_Ar.md) ·
  [سكربتات Lua](Docs/Lua_Scripting_Ar.md).

القوالب: `Default`، `ThirdPerson`، `FPSStarter`، `Platformer2D`.

## 📁 هيكل المشروع

```
Engine/        23 نظامًا: Core، RHI، Rendering، ECS، Scene، Physics، Audio، AI…
Editor/        محرر ImGui + المشغّل العربي
Games/         ألعاب كاملة (TreeWorld)
Tools/         nf CLI، AssetCooker، ModelImporter، Player، ProjectTool
Samples/       ديمو قابل للتشغيل (CliffStory، VehicleDemo…)
Templates/     مشاريع البداية
Tests/         26 حزمة اختبار
Docs/          الأدلة والتصميم وملاحظات الإصدار
ThirdParty/    Jolt، Dear ImGui، Lua، miniaudio، stb، cgltf
packaging/     سكربتات المثبّت والنسخة المحمولة
```

## 🗺 خارطة الطريق

المراحل 1–38 موجودة في الشجرة (تصيير، محرر، فيزياء، سكربتات، شبكات، 2D،
تدمير، ظلال متتالية، SSAO، PBR، IBL، NavMesh، مركبات Jolt، تصويب، ديناميكيات
صوت، ملصقات، مقياس رسم، ودقة ديناميكية). 0.5.0 تغلق مرحلة الصورة واليد: FXAA،
ملصقات مسقطة، دقة فائقة، دقة ديناميكية، ADS، اهتزاز، ارتداد، وإعدادات لاعب.

التالي: توقيع عام موثوق، مثبّت داخل الإصدار، استكشاف Linux، وأمثلة إضافية.

[ROADMAP.md](ROADMAP.md) ·
[التصميم الكامل](Docs/SANAD_Engine_Complete_Design.md) ·
[فهرس التوثيق](Docs/README.md).

## 🤝 المساهمة

Fork → فرع → بناء → صفر أخطاء validation → Pull Request. الوسوم
**`good first issue`** مناسبة للمبتدئين (توثيق، أمثلة، تجربة المحرر).

نرحّب بمبرمجي C++ والرسوميات والفيزياء والصوتيات وواجهات المحرر وكتّاب
التوثيق. التفاصيل: [CONTRIBUTING.md](CONTRIBUTING.md) · [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## 🔐 الأمان

أبلغ عن الثغرات بشكل خاص عبر **Security → Report a vulnerability** في GitHub.
لا تفتح Issue عامًا يحتوي على استغلال فعّال. السياسة الكاملة والإصدارات المدعومة
وتنبيه الملفات غير الموقّعة في [`SECURITY.md`](SECURITY.md).

## 📜 الترخيص والشكر

Apache License 2.0 — راجع [LICENSE](LICENSE).

مبني على **Jolt Physics**، **Dear ImGui**، **Amiri**، **Lua 5.4**،
**miniaudio**، **stb**، **cgltf**، **.NET**، و**Vulkan SDK**.

</div>
