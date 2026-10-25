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

[**جديد 0.6.0**](RELEASE_NOTES_0.6.0.md) ·
[**تنزيل آخر حزمة (0.6.0)**](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.6.0) ·
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
> **0.6.0 إصدار عام، وليس بيتا.** يُنشر كـ**نسخة محمولة لويندوز x64** مبنية من
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

من صفحة [إصدار 0.6.0](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.6.0):

| الملف | ماذا تفعل |
|:---|:---|
| [`SANAD-0.6.0-Windows-x64-Portable.zip`](https://github.com/abdallah2183/SANAD-Engine/releases/download/v0.6.0/SANAD-0.6.0-Windows-x64-Portable.zip) | فك الضغط في أي مكان → شغّل `SANADEditor.exe` |
| [`SHA256SUMS.txt`](https://github.com/abdallah2183/SANAD-Engine/releases/download/v0.6.0/SHA256SUMS.txt) | بصمة SHA-256 للحزمة المحمولة |

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
> TreeWorld تأتي داخل حزمة **0.6.0** المحمولة تحت `Games/`، وعلى فرع **`main`**
> كمصدر. استخدم `build_nf.bat Release` لتشغيلها من المستودع، أو شغّل النسخة
> المعبّأة نفسها من الحزمة.

## ✨ جديد في 0.6.0

- **تغذية القتال من سطر واحد** — `Feedback: flash_time=0.06 flash_size=0.26
  tracer_time=0.1 tracer_length=7 tracer_width=0.03 impact_time=8
  impact_size=0.18 shot_volume=0.6 impact_volume=0.4` يحوّل أحداث الطلقة
  والإصابة التي ينشرها المحرك منذ المرحلة 49 إلى وميض فوهة لكل رشقة، وشعاع لكل
  حبيبة، وعلامة سخام مسقطة على كل سطح تُصيبه الرصاصة.
- **بلا ممر GPU جديد وبلا أصل فني** — الوميض والأشعة كائنان في `RenderWorld` على
  مكعب إجرائي واحد مشترك، والعلامة decal قائم، والطلقة والأثر صوتان مولّدان في
  الذاكرة، فلا تحتاج اللعبة ملف WAV لتسمع سلاحها.
- **اختياري بتوافق كامل** — مشهد لا يذكر السطر يرسم ويخلط بالضبط ما كان في
  0.5.0. المكوّن يدور حفظًا/تحميلًا عبر الكاتب والقارئ الحقيقيين، والقيمة
  المستحيلة تُرفض باسم مفتاحها، والمدقق يحذّر من القيم خارج المدى.
- **الحدث يصل حيث يراه اللاعب** — المخرج يستهلك أحداث الإطار السابق بعد
  `begin_frame()`، ويشيخ قبل الاستيعاب، ويحتفظ بالإشارات حتى تُخلط كتلة صوتية
  كاملة، فلا تضيع أول طلقة في المعركة ولا تُسمع متأخرة.
- **`Ballistics.nfscene` يعرضها** — مشهد العرض صار مشتركًا، فرصاص السلاح الفعلي
  والتصادم يُرى ويُسمع من أول تشغيل.
- **كل ما في 0.5.0 باقٍ** — FXAA على صورة tonemap، والتصويب ADS، وأزرار الفأرة،
  واهتزاز الكاميرا والارتداد، والملصقات، ومقياس الرسم، والدقة الديناميكية،
  وديناميكيات الماستر، وإعدادات اللاعب.
- **توثيق يطابق البناء** — [ملاحظات الإصدار](RELEASE_NOTES_0.6.0.md) و
  [ملاحظات الديمو](Docs/Demo_Release.md) ودليل بدء سريع.

## 🔬 قياس زمن GPU (شجرة التطوير)

شجرة التطوير تغلق فجوة القياس في RHI بـ**استعلامات طوابع زمنية Vulkan حقيقية**.
القياس اختياري في الألعاب: اللعب العادي لا يسجّل طوابع ولا يدفع كلفة قياس لكل إطار.

- **المحرر:** القياس مفعّل افتراضيًا كأداة تطوير. لوحة الأداء تعرض
  `GPU (timestamp)` ومتوسطًا متحركًا وصفًّا لكل ممرّ. وإذا لم يدعم الجهاز
  الطوابع يبقى تقدير الإرسال حتى السياج (`gpu_us`/`gpu_avg_us`) بديلًا.
- **اللاعب:** `--gpu-timing` أو `NF_PLAYER_GPU_TIMING=1` يطبعان عيّنة الإطار
  الأخيرة وتفصيل الممرّات عند الخروج. بدونهما لا يُطبع تقرير، لكن مشهدًا يطلب
  صراحةً دقة ديناميكية بزمن GPU قد يفعّل طوابع الإطار داخليًا.
- **الدقة الديناميكية تقرأ GPU:** `dynamic_resolution_timing=gpu` يجعل الحاكم
  يستهلك زمن إطار GPU الحقيقي بدل زمن المعالج، مع رجوع تلقائي إلى المعالج عند
  غياب عيّنة جديدة. يطلب طوابع الإطار فقط — لا تصريف كل ممرّ — والتباطؤ يبقى
  بزمن الجدار.
- **مفتاحان مستقلان:** زمن الإطار، وزمن كل ممرّ. `Runtime` يفعّل الاثنين عبر
  `set_gpu_profiling_enabled` ويعيد تطبيقهما بعد كل تهيئة للعارض. القياس لكل
  ممرّ يضيف `BottomOfPipe` فيسلسل GPU، لذلك يبقى اختياريًا.
- **أرقام صادقة:** النتيجة تُقرأ بعد إطار واحد، وأول إطار لا يملك عيّنة.
  أزمنة الممرّات تتضمن كلفة التصريف، وحواجز RenderGraph خارج كل ممرّ.
- **الاختبارات:** `RHITests` يغطي قياس نقل GPU حقيقيًا، وتوافر النتائج،
  وتأطير الممرّات، وتكاملًا من ثلاثة إطارات، وقواعد الحاكم (حمل GPU مع تباطؤ
  الجدار). ويثبت `RuntimeTests` أن مشهدًا يفعّل طوابع الإطار بلا تفعيل القياس،
  وأن العيّنة القديمة تعيد الحاكم إلى المعالج. التفاصيل في
  [دليل قياس GPU](Docs/GPU_Profiling.md).

```bat
:: قياس تطويري؛ NFPlayer يقبل أيضًا NF_PLAYER_GPU_TIMING=1
build\Release\bin\NFPlayer.exe --project Games\TreeWorld\TreeWorld.nfproj --gpu-timing
```

### انتظام الإطارات (المرحلة 41)

تعرض لوحة الأداء في المحرر، و`NFPlayer --frame-stats`، نافذة متدحرجة من 120
إطارًا: متوسط FPS، وأدنى ١٪، وتفاوت الإطارات، وحكم عنق الزجاجة بين المعالج
والمعالج الرسومي. القياس بزمن الجدار الخام، لذلك يظهر انتظار العرض والتزامن
الرأسي؛ أضف `--gpu-timing` لزمن GPU والحكم. هذا قياس فقط ومنفصل عن الدقة
الديناميكية. التفاصيل في [انتظام الإطارات](Docs/Frame_Pacing.md).

```bat
build\Release\bin\NFPlayer.exe --project Games\TreeWorld\TreeWorld.nfproj --frame-stats --gpu-timing
```

### الضرر الطبقي والانفجارات وأحداث الإطلاق والحالات المستمرة وذاكرة القتال وتغذية القتال (المراحل 50–56)

شجرة التطوير تحوّل رقم الضرر الواحد إلى إصابة متعددة الطبقات. مناطق الإصابة
اختيارية لكل هدف (`Health: ... zones=true head=2 limb=0.75`)، والدرع مخزون ثانٍ
حقيقي (`armor=40 armor_max=40 absorb=0.6`)، و`pierce=` على السلاح يتجاوز جزءًا
منه. أنواع الضرر (`damage_type=ballistic|explosive|fire`) تقابلها مقاومة لكل نوع
(`resist_fire=0.4`). الترتيب ثابت ومختبَر: التوهين، المنطقة، المقاومة، الدرع،
الصحة. و`nf.impact(i)` يحمل `zone` و`armor_absorbed` و`damage_type` و`splash`.

ويمكن للسلاح أن يملك انفجارًا شعاعيًا (`blast_radius=4 blast_damage=45
blast_min=0.2`). الضرر يتلاشى خطيًا إلى الحافة، والمسافة تُقاس إلى أقرب نقطة من
الصندوق، والجدران تحجب الانفجار، وقواعد الفصائل تبقى، والهدف المباشر لا يُصاب
مرتين. كل ضحية أخرى تحصل على حدث إصابة مستقل بـ`splash=true`.

`ShotEvent` هو النصف الآخر: حدث لكل حبيبة، بنقطة خروج الفوهة والاتجاه بعد
التشتت ورقم الحبيبة. `nf.shot_count()` و`nf.shot(i)` يتيحان رسم وميض الفوهة أو
tracer حتى عندما تخطئ الطلقة.

ويمكن للإصابة أن تبدأ حالة مستمرة (`status=burn status_dps=4 status_time=3
status_tick=0.5`). الحرق والنزيف والتآكل ينبضون بإيقاع ثابت عبر نفس مسار
المقاومة والدرع، والنبضة الأخيرة الجزئية تدفع ما تبقّى بالضبط.
`nf.apply_status()` و`nf.status_remaining()` و`nf.clear_status()` متاحة من Lua.

وذاكرة القتال تبقى بعد الإطار: سجل ضرر يحفظ آخر مهاجم والمجاميع والقاتل،
و`retaliate=true` يجعل الوكيل يفضّل مهاجمه الأخير إن بقي هدفًا صالحًا. الرصاص
القريب يبني كبتًا يبطئ الاستدارة ويضيّق مخروط الإطلاق ويحبس الزناد فوق `0.75`.
Lua يقرأ ذلك عبر `nf.last_attacker()` و`nf.killer()` و`nf.damage_taken()`
و`nf.suppression()`. التفاصيل في [Combat_Damage.md](Docs/Combat_Damage.md).

```text
Health: max=100 health=100 armor=40 armor_max=40 absorb=0.6 zones=true head=2 limb=0.75 resist_explosive=0.2
Weapon: mode=semi rpm=60 magazine=1 damage=20 range=120
        delivery=projectile speed=35 gravity=9.81 damage_type=explosive
        blast_radius=4 blast_damage=45 blast_min=0.2
```

### تغذية القتال (`Feedback:`، المرحلتان 56 و59)

الأحداث بيانات حتى يرسمها شيء. سطر `Feedback:` يشترك بالمشهد في النصف المرئي
والمسموع: وميض واحد وصوت طلقة واحد لكل رشقة، وشعاع لكل حبيبة، وعلامة سخام
وصوت أثر لكل إصابة سطح حقيقية. نبضات الحالة وضحايا الانفجار مستثناة، لأنها ليست
رصاصة جديدة على سطح.

```text
Feedback: flash_time=0.06 flash_size=0.26 tracer_time=0.1 tracer_length=7
          tracer_width=0.03 impact_time=8 impact_size=0.18
          shot_volume=0.6 impact_volume=0.4
          blast_time=0.16 blast_size=0.9
          impact_volume_explosive=0.75 impact_volume_fire=0.55
          light_intensity=3 light_radius=7 light_r=1 light_g=0.72 light_b=0.35
          max_flashes=48 max_tracers=96 max_impacts=192 max_sounds=16 max_lights=4
```

المرحلة 59 أغلقت الثغرات الخمس التي أعلنها 0.6.0:

- **الوميض يضيء العالم.** كل وميض فوهة أو انفجار حيّ يضيف ضوءًا نقطيًا عابرًا،
  فالطلقة تُضيء الجدار الذي أُطلقت نحوه بدل أن تكتفي بمكعب انبعاثي. `light=false`
  يعيد مظهر 0.6.0 بالضبط، والضوء يأخذ ما تبقّى من خانات العارض بعد أضواء المشهد
  ويموت مع المكعب الذي وُلد منه.
- **الانفجار يبدو انفجارًا.** الإصابة الانفجارية تولّد وميضًا ثانيًا أكبر وأبطأ
  عند نقطة الإصابة — مرة واحدة لكل قذيفة، لا مرة لكل ضحية انفجار، فقنبلة وسط
  حشد تبقى كرة نار واحدة.
- **صوت الإصابة يتبع نوع الضرر.** رصاص على جدار وانفجار ونار ثلاثة أصوات إجرائية
  مختلفة بجهارة مستقلة، لأن حدث الإصابة يحمل `damage_type` منذ المرحلة 52، وصوت
  واحد للثلاثة كان يجعلها متطابقة.
- **السقوف صارت مفاتيح مشهد.** 48 وميضًا / 96 شعاعًا / 192 علامة / 16 صوتًا كانت
  أرقامًا ثابتة في الكود، وصارت قابلة للتأليف، مع `max_lights` لضوء الفوهة.
- **يوجد قسم في المفتش.** تغذية القتال كانت آخر مكوّن لا يُؤلف إلا بتحرير ملف
  المشهد يدويًا. اللوحة تُرفق وتحرّر وتتحقق من المدى نفسه الذي يفرضه المحمّل
  وتفصل. ووضع اللعب ونسخ الكيانات يحملان المكوّن أيضًا، فلم يبقَ مشهد يفقد
  تغذيته عند الدخول في وضع اللعب.

لا ممرّ رسم جديد ولا ملف صوت. الوميض والأشعة كائنان على مكعب إجرائي مشترك،
والعلامة decal مسقط، والأصوات مولّدة في الذاكرة ببذرة ثابتة. المحرك يحتفظ
بالأصوات الحية بين الكتل الصوتية ويطبّق الحجب ومستويات الناقل، ويتقاعد الصوت عند
انتهائه. مشهد بلا سطر `Feedback:` يرسم ويخلط بالضبط ما كان يفعله قبل 0.6.0،
والمدقق يذكر اسم المفتاح الخارج عن المدى. `Ballistics.nfscene` مشترك افتراضيًا.

## 🧱 المزايا

| المجال | ما يقدّمه |
|:---|:---|
| **التصيير** | Vulkan 1.2+، RenderGraph، PBR مؤجل (GGX)، ظلال متتالية، سماء إجرائية، IBL، SSAO، Bloom، اختيار GPU، LOD |
| **القياس** | طوابع وقت Vulkan اختيارية لزمن الإطار وكل ممرّ، دقة ديناميكية تستهلك زمن GPU مع رجوع إلى المعالج، قراءة غير حاجزة بعد إطار، تفصيل في المحرر، ونافذة انتظام إطارات متدحرجة (متوسط/أدنى ١٪/تفاوت) مع `NFPlayer --gpu-timing` و`--frame-stats` |
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
- **26 حزمة اختبار.** حزم 0.6.0 نجحت على ويندوز x64 (Release): `RHITests`
  345/345، `RuntimeTests` 364/364، `EditorTests` 289/289، `UITests` 46/46،
  `ToolTests` 55/55، `AudioTests` 136/136، و`ctest -j 4` يمرّ 26/26. وعلى شجرة
  التطوير بعد المرحلة 59 صارت الأرقام `RuntimeTests` 372/372 و`EditorTests`
  290/290 و`AudioTests` 138/138. على جهاز يمنع Smart App Control تشغيل ثنائي
  موقّع حديثًا قد تُسجَّل حزمة كعملية لم تبدأ حتى تحدّث خدمة السمعة رأيها - سياسة
  بيئة لا فشل اختبار.
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
> على جهاز فيه Inno Setup. إصدار 0.6.0 ينشر الـ zip المحمول فقط.

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

المراحل 1–49 موجودة في الشجرة (تصيير، محرر، فيزياء، سكربتات، شبكات، 2D،
تدمير، ظلال متتالية، SSAO، PBR، IBL، NavMesh، مركبات Jolt، تصويب، ديناميكيات
صوت، ملصقات، مقياس رسم، ودقة ديناميكية). 0.5.0 تغلق مرحلة الصورة واليد: FXAA،
ملصقات مسقطة، دقة فائقة، دقة ديناميكية، ADS، اهتزاز، ارتداد، وإعدادات لاعب.
0.6.0 تغلق فجوة التغذية في القتال: أحداث الطلقة والإصابة تصير وميض فوهة وشعاعًا
وعلامة سخام وصوتًا إجرائيًا، اختياريًا من سطر مشهد واحد.
وبعدها السلاح والقتال: سطر `Weapon:` بآلة حالة كاملة، نقاط صحة، ووكلاء قتال
يستديرون ويطلقون رشقات. المرحلة 46 تضيف مقذوفات بوقت طيران وسقوط، وفصائل
تمنع ضرر الحلفاء، ولوحة تأليف للوكيل والفصيل؛ والمرحلة 47 تجعل الوكيل يختار
أقرب عدو مرئي من فريق مختلف بدل اسم واحد؛ والمرحلة 48 تضيف استباق الهدف
المتحرك بمقذوف بطيء عبر حل تقاطع رياضي؛ والمرحلة 49 تعرض كل إصابة كحدث
مستهلك من Lua لأجل الآثار والأصوات.

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
