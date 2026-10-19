# SANAD Engine 0.2.0 — Demo Release | النسخة التجريبية

محرر ألعاب ثلاثي الأبعاد بواجهة عربية أولًا، مبني بـ C++23 وVulkan.
A 3D game editor with an Arabic-first UI, built in C++23 and Vulkan.

---

## ما الذي يشمله العرض؟ | What's in the demo

- **SANADEditor** — المحرر: مشهد، مفتش، شجرة كيانات، نظام ملفات، دليل بداية.
- **NFGamePlayer** — تشغيل اللعبة في نافذة مستقلة.
- **nf** — أداة الطرفية: إنشاء مشروع، طبخ، بناء حزمة.
- **NFAssetCooker / NFModelImporter** — تحويل الأصول والنماذج.
- **قوالب جاهزة**: Default، ThirdPerson، FPSStarter، Platformer2D.
- **أدلة**: البدء السريع، درس 20 دقيقة، Lua، استيراد الأصول.

## أول لعبة في خمس دقائق | Your first game in five minutes

1. شغّل **SANADEditor.exe**.
2. **Create New Project** → اختر **Third-Person Starter** → اسم → **Create Project**.
3. الكارت **«ابدأ من هنا»** بيشرح نفس المسار:
   1. مشهد جديد (أرضية + شمس + كاميرا)
   2. إضافة أرضية
   3. تشغيل اللعبة
4. **Play ▶** (أقصى يمين شريط الأدوات) يفتح اللعبة في نافذة مستقلة:
   `WASD` أو الأسهم للحركة، `Space` للقفز. اقفل النافذة للإيقاف.
5. `Ctrl+S` للحفظ. عند الإغلاق مع تغييرات غير محفوظة يظهر:
   **Save / Discard / Cancel**، وتُكتب دائمًا نسخة استرداد في
   `Cache/EditorRecovery.nfscene`.

### بناء لعبة مستقلة | Standalone build

```bat
nf new MyGame --template ThirdPerson
nf build --project MyGame\MyGame.nfproj
MyGame\dist\NFPlayer.exe
```

## المتطلبات | Requirements

- Windows 10/11 **64-bit** فقط.
- تعريف GPU يدعم **Vulkan 1.2+** (`vulkan-1.dll` يأتي مع التعريف، وليس مع الحزمة).
- مساحة تقريبًا 20 MB بعد فك الضغط.

## قيود معروفة في هذه النسخة | Known limitations

- **Windows فقط**؛ لا Linux/macOS بعد.
- **الملفات غير موقّعة (unsigned)**: إذا كان **Smart App Control** أو سياسة
  **WDAC** مفعّلة على جهازك، قد يرفض Windows تشغيل المحرر. الحلول:
  1. إيقاف Smart App Control من
     `Windows Security → App & browser control → Smart App Control`، أو
  2. توقيع الملفات بشهادة code-signing قبل التوزيع العام.
- مشهد الديمو بسيط بصريًا (إضاءة/مواد أساسية)؛ النسخة القادمة تحسّن العرض.
- النسخ الاحتياطي التلقائي مسار استرداد فقط، وليس نظام نسخ كامل.

## الإبلاغ عن مشكلة | Reporting a problem

اذكر:
1. الخطوة التي توقفت عندها.
2. نص الـ console (المحرر يطبع كل أخطاء Vulkan/الأصول).
3. نسخة Windows ونوع كرت الشاشة.

---

*هذه نسخة تجريبية (demo) وليست إصدارًا نهائيًا. الواجهة والمسارات قابلة للتغيير.*
