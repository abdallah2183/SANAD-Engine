# المساهمة في محرك سَنَد | Contributing to SANAD Engine

[دليل المساهمة بالعربية](#دليل-المساهمة-باللغة-العربية) | [English Guide](#english-contributing-guide)

---

## دليل المساهمة باللغة العربية

أهلاً بك في مجتمع **محرك سَنَد (SANAD Engine)** بقيادة وتأسيس **عبدالله**. يسعدنا انضمامك للمساهمة في بناء وتطوير هذا الصرح التقني لخدمة مجتمع مطوري الألعاب.

### مبادئ وفلسفة المشروع
- **مشروع تكاملي:** المحرك مبني ليكون إطار عمل جامعاً للكفاءات البرمجية العربية، بما يمكن المطورين من بناء ألعابهم باستقلالية وحرية تقنية كاملة.
- **الانضباط المعماري:** كل إضافة برمجية تخضع لاختبارات دقيقة وتدقيق لعزل الطبقات البرمجية (Layering Architecture).
- **خلو تام من الأخطاء:** الالتزام الصارم بعدم وجود أي تحذيرات أو أخطاء في طبقات فحص Vulkan Validation Layers أو تسريبات في الذاكرة.

---

### مجالات المساهمة المتاحة

1. **الرسوميات والتظليل (Graphics & Shaders):**
   - تطوير خرائط الظلال الاتجاهية (Directional Shadows / PCF).
   - سماء إجرائية وحسابات تشتت الضوء (Atmospheric Scattering).
   - تأثيرات ما بعد المعالجة (Bloom, Tonemapping, FXAA/TAA, SSAO).

2. **الصوتيات (Audio Systems):**
   - استكمال دمج مكتبة MiniAudio كواجهة فعلية لمكتبة الصوت خلف تجريد `AudioDevice`.
   - دعم استيراد وتشغيل ملفات WAV و OGG.

3. **المحاكاة الفيزيائية (Physics):**
   - دمج محرك Jolt Physics خلف واجهة `PhysicsWorld` للمحاكاة المعقدة.
   - تصادمات الشبكات والمجسمات المعقدة (Convex Hull & Mesh Collision).

4. **لغات البرمجة والسكربت (Scripting):**
   - إضافة ربط للغة C# عبر .NET أو Lua عبر Sol2.
   - إتاحة برمجة ميكانيكا الألعاب دون الحاجة لإعادة بناء المحرك.

5. **واجهة المحرر والتعريب (Editor & Arabic UI):**
   - إدماج خطوط عربية وتشكيل الكلمات (Text Shaping عبر HarfBuzz أو FreeType).
   - دعم التخطيط من اليمين لليسار (RTL Support) في واجهة Dear ImGui.
   - تحسين أدوات المقابض الحركية (Gizmos) والمعاينة الفورية.

6. **إدارة الأصول ومستوردات النماذج (Asset Pipelines):**
   - بناء مستورد لنماذج glTF 2.0 و FBX وتحويلها إلى صيغة المحرك `.nfmesh`.
   - مستورد متقدم للمواد والخامات PBR.

7. **التوثيق والأمثلة (Documentation & Samples):**
   - إعداد شروحات ودروس برمجية تفصيلية.
   - بناء أمثلة ألعاب مصغرة توضح مزايا المحرك.

---

### خطوات إرسال المساهمة (Pull Request Workflow)

1. **عمل Fork للمستودع:**
   اضغط على زر `Fork` في أعلى صفحة المستودع على GitHub.

2. **استنساخ المستودع محلياً:**
   ```bash
   git clone https://github.com/<YOUR_USERNAME>/SANAD-Engine.git
   cd SANAD-Engine
   ```

3. **إنشاء فرع جديد للميزة:**
   ```bash
   git checkout -b feature/your-feature-name
   ```

4. **بناء المشروع واختباره:**
   تأكد من نجاح البناء ومرور جميع الاختبارات:
   ```cmd
   build_nf.bat
   .\build\DebugNinja\bin\EditorTests.exe
   ```

5. **قواعد المعمارية الأساسية:**
   - مكتبة `Engine/Assets` مستقلة تماماً ولا تعتمد على `NFRendering` أو الـ RHI.
   - مكتبة `Engine/Rendering` لا ترتبط مباشرة بـ `NFEcs` أو `NFScene`؛ الربط يتم دائماً في طبقة `NFRuntime`.
   - الالتزام بمعيار C++23.

6. **إرسال الـ Pull Request:**
   ادفع تعديلاتك إلى فرعك وافتح Pull Request مع وصف واضح ومفصل للتغييرات.

---

## English Contributing Guide

Welcome to the **SANAD Engine** community, founded and led by **Abdallah**. We welcome developers, graphics engineers, audio programmers, and technical writers to build this engine together.

### Architectural Principles
- **Strict Layer Invariants:** Subsystems are decoupled behind abstract interfaces. `Assets` never links `Rendering`; `Rendering` never links `ECS`/`Scene`.
- **Zero Validation Errors:** Code touching the Vulkan RHI must run with zero Vulkan validation warnings and zero resource leaks.
- **Deterministic Stepping:** The runtime simulation loop (`Physics`, `Animation`, `Audio`, `Gameplay`) must remain deterministic and testable in headless mode.

### Getting Started
1. **Fork and clone** the repository.
2. Create a dedicated feature branch: `git checkout -b feature/your-feature-name`.
3. Verify your build passes all tests using `build_nf.bat` or the CMake toolchain.
4. Submit a **Pull Request** detailing your changes.

---

<p align="center">
  <strong>المؤسس والقائم على العمل:</strong> <a href="https://github.com/abdallah2183"><strong>عبدالله (Abdallah)</strong></a><br>
  <em>Founder & Project Lead: Abdallah (@abdallah2183)</em>
</p>
