# Contributing | المساهمة

Thanks for your interest in SANAD / محرك سَنَد. Contributions of code,
docs, samples, and Arabic-first UX are all welcome.

## How to contribute

1. **Fork & clone**, then branch: `git checkout -b feature/my-change`
2. **Build & test** before opening a PR:
   ```bat
   build_nf.bat
   .\build\DebugNinja\bin\RuntimeTests.exe
   ```
3. **Open a Pull Request** describing what changed and why.

Looking for a place to start? Issues tagged **`good first issue`** are
beginner-friendly (docs, samples, editor UX).

## Ground rules

- **Layer invariants:** `Assets` never links `Rendering`; `Rendering` never
  links `ECS`/`Scene` — binding happens only in `NFRuntime`. Enforced by
  `Scripts/check_layering.sh`.
- **Zero validation errors / zero leaks** for anything touching the Vulkan RHI.
- **Deterministic stepping:** the runtime simulation loop stays deterministic
  and headless-testable.
- **C++23**, `/W4 /WX` — a warning is a failed build.
- **Behavior must be tested, not described.** A feature is done when a suite
  proves it; "the code exists" is not the claim.

## Contribution areas

Graphics & shaders · physics & destruction · scripting (Lua/C#) · audio ·
networking · **Arabic RTL editor & localization** · asset pipelines · docs
& samples.

---

<div dir="rtl">

## المساهمة

شكراً لاهتمامك بـ **محرك سَنَد**. نرحّب بمساهمات الكود والتوثيق والأمثلة
وتجربة المحرر العربية.

**الخطوات:** انسخ المستودع (Fork) → أنشئ فرعاً → ابنِ واختبر (`build_nf.bat`
ثم شغّل مجموعات الاختبار) → افتح Pull Request بوصف واضح.

**القواعد:** فصل الطبقات صارم (`Scripts/check_layering.sh`) · صفر أخطاء تحقق
Vulkan وصفر تسريبات · خطوات المحاكاة حتمية · C++23 مع `/W4 /WX` · كل ميزة
يجب أن يُثبتها اختبار، لا أن تُوصَف فقط.

**المجالات:** الرسوميات والتظليل · الفيزياء والتدمير · البرمجة (Lua/C#) ·
الصوت · الشبكات · **المحرر العربي والتعريب** · خطوط الأصول · التوثيق والأمثلة.

</div>

---

<sub>Project lead: [Abdallah (@abdallah2183)](https://github.com/abdallah2183)</sub>
