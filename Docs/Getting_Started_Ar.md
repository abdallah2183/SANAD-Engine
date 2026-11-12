# البدء السريع

[التوثيق](README.md) · [English](Getting_Started.md)

هذا الدليل للنسخة المحمولة **0.8.0** على Windows. للبناء من الشيفرة، راجع [دليل البناء والاختبارات](Build_and_Test.md).

## المتطلبات

- Windows 10 أو 11 بنواة x64.
- تعريف بطاقة رسومية يدعم Vulkan.
- مجلد قابل للكتابة لمشاريعك.

## 1. تنزيل المحرك

نزّل الحزمة المحمولة وملف `SHA256SUMS.txt` من [صفحة 0.8.0](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.8.0)، ثم فك الضغط وشغّل `SANADEditor.exe`.

الحزمة تحمل توقيع تطوير ذاتيًا. إذا ظهرت رسالة عن الناشر أو سياسة Windows، راجع [الدعم](../SUPPORT.md#windows-blocks-a-downloaded-executable).

## 2. إنشاء مشروع

اختر **إنشاء مشروع جديد**، ثم قالب **ThirdPerson**. اكتب اسمًا مثل `MyGame` وحدد مجلدًا قابلًا للكتابة، ثم أنشئ المشروع. يفتح المحرر مشهد البداية مباشرة.

اضغط **تشغيل** لتجربة المشروع، وأغلق نافذة اللعبة للعودة إلى التحرير. القالب بداية قابلة للتعديل، وليس لعبة نهائية.

## 3. أول تعديل

اختر عنصرًا من شجرة المشهد، ثم غيّر موضعه من المفتش أو باستخدام أداة التحريك. احفظ بـ`Ctrl+S`. التراجع بـ`Ctrl+Z`، والإعادة بـ`Ctrl+Y`.

لشرح الواجهة خطوة بخطوة، انتقل إلى [الدرس العملي](Tutorial_Ar.md). تفاصيل اللوحات موجودة في [دليل المحرر](Editor_Guide.md).

## 4. البناء والمشاركة

لتصدير اللعبة بملف EXE واحد من بناء المحرك الحالي، راجع [التصدير والنشر على Windows](Windows_Export.md#التصدير-والنشر).

من مجلد المحرك المحمول:

```bat
nf.exe new MyGame --name MyGame --template ThirdPerson
nf.exe build --project MyGame\MyGame.nfproj
MyGame\dist\NFPlayer.exe --project MyGame\dist\MyGame.nfproj
```

إذا أنشأت المشروع مسبقًا من الواجهة، نفّذ أمر `build` على ملف مشروعك فقط. شارك مجلد `dist` كاملًا حتى تصل الأصول والشيدرز مع اللاعب.

## 5. التحقق دون نافذة

```bat
nf.exe run --project MyGame\MyGame.nfproj --frames 120 --headless --validation
```

النتيجة الصحيحة: رمز خروج `0` ورسالة `Validation errors: 0`.

## أين تعمل؟

| المسار | وظيفته |
|:---|:---|
| `MyGame.nfproj` | وصف المشروع ومشهد البداية ونقاط تركيب الملفات |
| `Content/Scenes/` | المشاهد التي تعدّلها |
| `Content/Scripts/` | سكربتات Lua |
| `Content/Materials/` | المواد |
| `Cache/` | بيانات يولدها طبخ الأصول |
| `dist/` | الحزمة المستقلة الناتجة عن البناء |

عدّل ملفات المصدر داخل `Content/`، ثم أعد البناء لتحديث `dist/`.

الخطوة التالية: [Lua](Lua_Scripting_Ar.md) · [استيراد الأصول](Asset_Import.md) · [العمل مع وكيل برمجي](AI_Game_Guide.md).
