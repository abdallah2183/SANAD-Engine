# المساهمة في محرك سَنَد | Contributing to SANAD Engine

[العربية](#-دليل-المساهمة-باللغة-العربية) | [English](#-english-contributing-guide)

---

## 🇸🇦 دليل المساهمة باللغة العربية

أهلاً بك في مجتمع **محرك سَنَد (SANAD Engine)** بقيادة وتأسيس المطور **عبدالله**! يسعدنا جداً اهتمامك بالمساهمة في بناء وتطوير هذا الصرح التقني ليكون سنداً حقيقياً لمطوري الألعاب في الوطن العربي.

### 🌟 فلسفة محرك سند
- **سند لكل مطور عربي:** المحرك مبني ليكون مشروعاً تعاونياً جامعاً لكل الكفاءات البرمجية العربية، لنمكّن المطورين من بناء ألعابهم بحرية واستقلالية كاملة.
- **الجودة والصرامة البرمجية:** كل سطر كود يضاف يمر عبر فحوصات واختبارات مؤتمتة وتدقيق لطبقات المعمارية (Layering Architecture).
- **صفر أخطاء وفاليديشن:** نحرص على ألا يحتوي الكود على أي أخطاء من طبقات فحص Vulkan Validation Layers أو أي تسريب للذاكرة.

---

### 🎯 ما هي المجالات التي يمكنك المساهمة فيها الآن؟

1. **الرسوميات وتظليل المشاهد (Graphics & Shaders):**
   - تطوير خرائط الظلال الاتجاهية (Directional Shadows / PCF).
   - سماء إجرائية (Procedural Atmosphere & Sun Disk).
   - تأثيرات ما بعد المعالجة (Bloom, Tone Mapping, FXAA/TAA, Screen Space Ambient Occlusion).

2. **الصوتيات (Audio Systems):**
   - إكمال دمج مكتبة `MiniAudio` كـ Backend حقيقي لمكتبة الصوت خلف واجهة `AudioDevice`.
   - استيراد وتشغيل ملفات WAV و OGG و MP3.

3. **الفيزياء (Physics):**
   - دمج محرك `Jolt Physics` خلف واجهة `PhysicsWorld` لدعم المحاكاة المعقدة.
   - تصادمات الأشكال المعقدة (Convex Hull / Triangle Mesh Collision).

4. **لغات البرمجة والسكربت (Scripting):**
   - إضافة ربط للغة **C#** (باستخدام .NET 8/9 أو mono/coreclr) أو **Lua** عبر Sol2.
   - إتاحة برمجة ميكانيكا الألعاب دون الحاجة لإعادة ترجمة المحرك بالكامل.

5. **واجهة المحرر والتعريب (Editor & Arabic UI):**
   - إدماج خطوط عربية وتشكيل الكلمات (Text Shaping عبر HarfBuzz أو FreeType).
   - دعم التخطيط من اليمين لليسار (RTL Support) في واجهة Dear ImGui.
   - تحسين أدوات تحريك الكائنات (Gizmos) والمعاينة الفورية.

6. **إدارة الأصول والمستوردات (Asset Pipelines):**
   - بناء مستورد نماذج بصيغة **glTF 2.0** و FBX وتحويلها إلى `.nfmesh`.
   - مستورد مواد وخامات متقدم.

7. **التوثيق والأمثلة (Documentation & Samples):**
   - كتابة شروحات برمجية ودروس باللغة العربية.
   - إنشاء نماذج ألعاب مصغرة (Mini-games) توضح إمكانيات المحرك.

---

### 🛠️ خطوات إرسال مساهمتك (Pull Request Workflow)

1. **قم بعمل Fork للمستودع:**
   اضغط على زر `Fork` في أعلى صفحة المستودع على GitHub.

2. **استنسخ نسختك محلياً:**
   ```bash
   git clone https://github.com/<YOUR_USERNAME>/NOVAForge-Engine.git
   cd NOVAForge-Engine
   ```

3. **أنشئ فرعاً جديداً لميزتك:**
   ```bash
   git checkout -b feature/my-cool-feature
   ```

4. **قم بالبناء واختبار التعديلات:**
   تأكد من نجاح البناء ومرور جميع الاختبارات:
   ```cmd
   build_nf.bat
   .\build\DebugNinja\bin\EditorTests.exe
   ```

5. **قواعد المعمارية الهامة:**
   - مكتبة `Engine/Assets` نقية ولا تعتمد أبداً على الرسوميات `NFRendering` أو الـ RHI.
   - مكتبة `Engine/Rendering` لا تعتمد على `NFEcs` أو `NFScene`؛ الربط يتم دائماً في طبقة `NFRuntime`.
   - الالتزام بمعيار C++23.

6. **أرسل الـ Pull Request:**
   ادفع تعديلاتك إلى فرعك في GitHub وافتح Pull Request مع وصف واضح ومفصل لما تم إنجازه. سنكون في غاية السعادة بمراجعته ودمجه!

---

<br>

## 🌐 English Contributing Guide

Welcome to the **SANAD Engine** community, founded and led by **Abdallah**! We are thrilled to welcome developers, graphics engineers, audio designers, tools programmers, and documentation writers from around the globe to build this next-generation engine together.

### 📐 Architectural Principles
- **Strict Layer Invariants:** Subsystems are decoupled behind abstract interfaces. `Assets` never links `Rendering`; `Rendering` never links `ECS`/`Scene`.
- **Zero Validation Errors:** Code touching the Vulkan RHI must run with zero Vulkan validation warnings and zero resource leaks.
- **Deterministic Stepping:** The runtime simulation loop (`Physics`, `Animation`, `Audio`, `Gameplay`) must remain deterministic and testable in headless mode.

### 🚀 Getting Started
1. **Fork & Clone** the repository.
2. Create a dedicated feature branch: `git checkout -b feature/your-feature-name`.
3. Verify your build passes all tests using `build_nf.bat` or the CMake toolchain.
4. Submit a **Pull Request** detailing your changes.

Thank you for contributing to the future of SANAD Engine! 🌟
<br>
*Founder & Project Lead: Abdallah (@abdallah2183)*
