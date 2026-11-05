# SANAD Engine 0.8.0 — محرر أكثر ثباتًا | Editor reliability update

Windows 10/11 x64 · C++23 · Vulkan · Arabic / English

[Download / التنزيل](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.8.0)

## التنزيل | Download

| File | Contents |
|:---|:---|
| `SANAD-0.8.0-Windows-x64-Portable.zip` | Editor, CLI, player, model importer, asset cooker, shaders, templates, Arabic font, documentation and TreeWorld source project |
| `SHA256SUMS.txt` | SHA-256 checksum of the portable ZIP |

Extract the ZIP and run `SANADEditor.exe`. A Vulkan-capable GPU driver is required. This release is portable; no installer is included. Executables carry the self-signed **SANAD Dev Build** development signature, which is not a publicly trusted signing certificate; Windows may still display a publisher warning.

فك الضغط وشغّل `SANADEditor.exe`. يلزم تعريف رسومي يدعم Vulkan. الإصدار محمول بلا مثبّت، وتوقيعه شهادة تطوير ذاتية باسم **SANAD Dev Build**؛ قد يعرض Windows تنبيهًا عن الناشر.

## أبرز التحسينات

- **المفتش ولوحات المشهد:** مزامنة الخصائص بعد التراجع والإعادة وفتح مشهد آخر، تنشيط تبويب اللوحة عند فتحها، وترتيب أوضح في المساحات الضيقة.
- **متصفح الملفات:** إظهار المجلدات الفارغة والمتداخلة، إبقاء التنقل بالمجلدات أثناء فلترة الملفات، تبديل الجذر دون انتظار انتهاء التخزين المؤقت، واستقرار سجل التنقل والمفضلة.
- **الملفات العربية:** الحفاظ على UTF-8 أثناء العرض والبحث وإعادة التسمية واختصار الأسماء الطويلة، وتصحيح تكوين مسارات الجذور.
- **التحرير والتراجع:** تحسين إنشاء الكائنات وحذفها وتكرارها واستعادة علاقات القوالب بعد التراجع والإعادة، ومعالجة تبديل المشاهد مع تغييرات غير محفوظة.
- **الكاميرا والإضاءة:** تحرير الكاميرا النشطة، إصلاح حقل هدف المتابعة، الاحتفاظ بتعديلات الظلال حتى تطبيقها، وإظهار قيم زوايا الشمس الفعلية.
- **ربط الأزرار:** بحث في الإجراءات، نافذة مستقلة لالتقاط الزر، إلغاء دون تغيير الربط، وعدم التقاط نقرة فتح النافذة أو النقر خارج منطقة الفأرة المخصصة.
- **السجل:** عدادات تراعي المرشحات وتشمل الأخطاء الحرجة، عرض ونسخ الرسائل متعددة الأسطر، تمرير يحترم موضع القراءة، وقائمة نسخ مستقرة عند وصول رسائل جديدة.
- **الاستيراد والتصدير:** حقول نصية قابلة للنمو، خيار استبدال صريح عند الاستيراد، وامتداد تصدير مطابق للصيغة المختارة دون تغيير النقاط في أسماء المجلدات.
- **العرض والتغليف:** إصلاح شفافية tonemapping وحالة هدف الرسم قبل الواجهة، وإرفاق الترخيص وملاحظات الإصدار، وحصر أمثلة الحزمة لمنع إدراج مشاريع وتجارب محلية.

## Highlights

- Scene panels and the Inspector refresh after undo/redo and scene replacement. Newly opened docked panels select their own tabs.
- The file browser exposes empty/intermediate directories, preserves navigation under file filters, switches mounts immediately and handles history/favourites safely.
- UTF-8 filenames remain intact through display, search, rename and shortening.
- Creation, deletion, duplication, prefab restoration and unsaved-scene switching have additional regression coverage.
- Camera editing follows the active camera; follow-target text is safely editable. Shadow drafts persist until Apply, and sun angle controls show their real values.
- Input rebinding uses a dedicated capture dialog with Cancel and an explicit mouse capture area. Action search and compact layouts improve navigation.
- The Console preserves multiline messages, includes Fatal in error counts, copies the visible query and allows reading older messages without forced scrolling.
- Import/export uses growing text fields, explicit import overwrite control and format-correct output extensions.
- Rendering fixes preserve viewport alpha and the correct backbuffer state for the UI pass.
- Includes the engine work through Phase 131: SSAO, atmospheric fog, terrain erosion and animation inertialization.

## Verification | التحقق

- **Release build succeeded** with the repository's warnings-as-errors configuration.
- **26 test suites:** **2,828 passed, 0 failed, 3 skipped**.
- Skipped: the opt-in million-entity benchmark, live XInput hardware without a connected controller, and the dedicated-server binary integration case because that binary is not built in this configuration.
- `EditorTests`: **338 passed**. `UITests`: **47 passed**.
- Static UI localization check passed.
- Native editor automation: **180 frames, automation OK, Vulkan validation errors: 0, live RHI objects at shutdown: 0**.

تم التحقق من تشغيل الحزمة المحمولة بعد فك الضغط، ومن إنشاء مشروع وبنائه وتشغيله دون نافذة مع التحقق الرسومي. بصمة الملف المنشور موجودة في `SHA256SUMS.txt`.
