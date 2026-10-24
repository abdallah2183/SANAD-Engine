# السكربت في محرّك سند (Lua) — دليل المبتدئ

> **السلوك = سكربت.** المحرّك يرسم المشهد، وأنت تكتب السكربت ليخلي الكائنات
> تعمل شيئًا: تتحرك، تتفاعل، تكسب، تخسر. هاد الدليل يعلّمك هاد الفنّ — من أوّل
> سطر لأول لعبة.

---

## 1. الفكرة الأساسية

كل كائن في المشهد يقدر يحمل **سكربت** — ملف لوا (`.lua`). السكربت يُنفَّذ كل
إطار، ويستطيع يقرأ ويغيّر حالة الكائن.

الدرس الوحيد اللي لازم تحفظو:

```lua
function update(dt)
    -- هون تكتب اللي بدّك يصير، كل إطار.
    -- dt = الزمن منذ آخر إطار بالثواني (مثلاً 0.016 = 1/60 ثانية).
end
```

- **دون `update`** = سكربت لا يعمل. خلّيها معرّفة دائمًا.
- **`dt`** = سرعة الزمن الحقيقية. استعملها عشان الحركة تبقى ناعمة على أي جهاز.

---

## 2. معرّف الكائن (self)

كل سكربت يعرف تلقائيًا على أنه "أنا" أي كائن يحملو. معرّف الكائن محفوظ في:

```lua
self_id    -- رقم الكائن
self_gen   -- رقم الجيل (لتفادي الخلط بين الكائنات المحذوفة والجديدة)
```

تقدر أيضًا تجيبهم بنفسك:

```lua
local id, gen = nf.self()
```

> كل دالة تتعامل مع كائن تبدأ بـ `(id, gen)` — لذلك نمرّر `self_id, self_gen`.

---

## 3. تحريك كائن

### القراءة
```lua
local x, y, z = nf.entity_pos(self_id, self_gen)
if x == nil then return end  -- الكائن بلا موضع بعد
```

### الكتابة
```lua
nf.set_entity_pos(self_id, self_gen, x, y, z)
```

### مثال: سقوط مستمر
```lua
function update(dt)
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if x == nil then return end
    nf.set_entity_pos(self_id, self_gen, x, y - 2 * dt, z)  -- ينزل 2 متر/ثانية
end
```

> **مهم:** تحرّك بالاعتماد على `dt` (مش رقم ثابت) عشان السرعة تبقى نفسها على
> كل الأجهزة. `y - 2 * dt` = نزول ثابت بالزمن، بينما `y - 0.02` = أبطأ على
> الأجهزة البطيئة.

---

## 4. الموجات والحركة الناعمة

أدوات جاهزة:

| الدالة | معناها |
|:--|:--|
| `math.sin(x)` / `math.cos(x)` | موجة بين -1 و 1 |
| `nf.clamp(v, lo, hi)` | حصر قيمة |
| `nf.lerp(a, b, t)` | تنقّل ناعم من `a` لـ `b` (t بين 0 و 1) |

### مثال: تمايل صعودًا ونزولًا
```lua
local home_y = 2
local clock  = 0

function update(dt)
    clock = clock + dt
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if x == nil then return end
    local new_y = home_y + math.sin(clock * 2) * 1  -- تمايل ±1 متر
    nf.set_entity_pos(self_id, self_gen, x, new_y, z)
end
```

> **`local clock = 0` فوق `update`** = ذاكرة السكربت. المتغيّرات المحليّة
> تُحفظ بين الإطارات، فتقدر تبني ساعة، عدّاد، أو حالة تتغيّر بالزمن.

---

## 5. دالة الطباعة (للتتبّع)

```lua
print("لقد تحركت! الموضع الحالي: ", x, y, z)
print(string.format("النقاط: %d", score))
```

- `print(...)` يوجّه لسجل المحرّك — يظهر بالطرفية/اللوج.
- استعملو بكثرة وقت التعلم: اطبع قيمك عشان تفهم شنّ يصير.

---

## 6. ربط السكربت بكائن

طريقتان:

### من المحرّر (الأسهل)
1. انقر الكائن في شجرة المشهد.
2. في المفتش، قسم **`السكربت`** ← أضف ملف `.lua` (أو اسحبو من متصفّح الأصول).

### يدويًا (من ملف المشهد `.nfscene`)
أضف سطرًا تحت الكيان:
```
  Script: lang=lua path=content://Scripts/example.lua
```
> المسار لازم يبدأ بـ `content://` ويشاور على ملف موجود في `Content/Scripts/`.
> ما في مسافات بالمسار.

> كل مشروع جديد يجي مع `Content/Scripts/example.lua` **مربوط أصلاً** — افتحو،
> عدّلو، واضغط "تشغيل".

---

## 7. مرجع الدوال الكامل

| الدالة | الوسائط | ترجع | الوصف |
|:--|:--|:--|:--|
| `nf.self()` | — | `id, gen` | معرّف الكيان الحالي |
| `nf.entity_pos` | `id, gen` | `x, y, z` | موضع الكيان |
| `nf.set_entity_pos` | `id, gen, x, y, z` | `bool` | تحرّك الكيان |
| `nf.set_entity_scale` | `id, gen, sx, sy, sz` | `bool` | تغيير حجم الكيان (صفر يخفيه) |
| `nf.entity_alive` | `id, gen` | `bool` | هل الكائن موجود؟ |
| `nf.find_entity` | `name` | `id, gen` أو `nil` | ابحث عن كائن باسمه |
| `nf.entity_name` | `id, gen` | نص أو `nil` | اسم الكائن |
| `nf.entity_count` | — | رقم | عدد الكائنات الحيّة |
| `nf.action` | `action` | `bool` | هل الفعل مضغوط؟ (`move_forward`، `jump`…) |
| `nf.axis` | `action` | رقم `-1..1` | محور حركة (`move_x`، `move_z`، `look_x`، `look_y`) |
| `nf.camera_move` | — | `x, z` | محورا الحركة مُدوَّران بـ yaw الكاميرا — حرّك بهما |
| `nf.camera_yaw` | — | رقم | دوران الكاميرا الفعّالة بالدرجات (`0` إن لا كاميرا) |
| `nf.shake` | `amount` | — | اهتزاز كاميرا بصدمة `0..1` (0.2 خطوة، 0.6 ضربة، 1.0 انفجار) |
| `nf.recoil` | `pitch, yaw` | — | ارتداد بالدرجات يُزاح على النظر ثم يتعافى إلى تصويب اللاعب |
| `nf.weapon_ammo` | — | رقم | الطلقات في المخزن (`0` إن لا سلاح) |
| `nf.weapon_reserve` | — | رقم | الطلقات في الاحتياط |
| `nf.weapon_reloading` | — | `bool` | هل التعبئة جارية؟ |
| `nf.weapon_reload` | — | `bool` | ابدأ تعبئة (يفشل إن كان المخزن ممتلئًا) |
| `nf.weapon_fire` | — | رقم | أطلق إن سمح الإيقاع؛ يرجع عدد الحبيبات المتتبَّعة |
| `nf.last_hit` | — | `hit, x, y, z, distance` | نتيجة آخر طلقة |
| `nf.impact_count` | — | عدد | عدد الإصابات في الإطار المكتمل السابق |
| `nf.impact` | `i` | جدول أو `nil` | إصابة واحدة: الهدف، الاسم، الموضع، الطبيعي، الضرر، `zone`، `armor_absorbed`، `damage_type`، `blocked`، `killed`، `splash` |
| `nf.shot_count` | — | عدد | عدد الطلقات في الإطار المكتمل السابق |
| `nf.shot` | `i` | جدول أو `nil` | طلقة واحدة: الرامي، نقطة الخروج، الاتجاه، الضرر، رقم الحبيبة وعددها، `projectile`، `seed` |
| `nf.health` | `id, gen` | `current, max` | نقاط صحة الكائن (`0, 0` إن لا صحة له) |
| `nf.damage` | `id, gen, amount` | رقم | ضرر صحة خام؛ يتجاوز الدرع ومناطق الإصابة |
| `nf.apply_status` | `id, gen, kind, dps, seconds, tick` | `bool` | أضف أو جدّد حالة `burn`/`bleed`/`corrode` |
| `nf.status_remaining` | `id, gen, kind` | رقم | الثواني المتبقية للحالة (`0` إن غابت) |
| `nf.clear_status` | `id, gen, kind` | `bool` | أزل حالة واحدة |
| `nf.last_attacker` | `id, gen` | `id, gen` أو `nil` | آخر من أصاب الكيان |
| `nf.killer` | `id, gen` | `id, gen` أو `nil` | من وجّه الضربة القاتلة |
| `nf.damage_taken` | `id, gen` | `health, armor, hits, seconds` | ذاكرة الضرر التراكمية |
| `nf.suppression` | `id, gen` | رقم `0..1` | كبت الوكيل من الرصاص القريب |
| `nf.clamp` | `v, lo, hi` | رقم | حصر قيمة |
| `nf.lerp` | `a, b, t` | رقم | تنقّل ناعم |
| `nf.log_info/warn/error` | رسالة | — | اكتب للسجل بمستوى |
| `print` | أي قيم | — | اطبع للسجل |
| `nf.version` | — | نص | إصدار الواجهة |

مثال استهلاك إصابات الإطار السابق:

```lua
function update(dt)
    for i = 1, nf.impact_count() do
        local hit = nf.impact(i)
        if hit and not hit.blocked then
            -- hit.x, hit.y, hit.z: نقطة الإصابة
            -- hit.nx, hit.ny, hit.nz: الطبيعي
            -- hit.damage / hit.applied / hit.killed
            -- hit.zone: "body" أو "head" أو "limbs"
            -- hit.armor_absorbed: ما امتصه الدرع
            -- hit.damage_type: "ballistic" أو "explosive" أو "fire"
            -- hit.splash: true لإصابة انفجار شعاعي
            -- hit.status: true لنبضة حالة مستمرة (حرق/نزيف/تآكل)
            nf.log_info("impact on " .. hit.name .. " (" .. hit.zone .. ")")
        end
    end
end
```

الإصابات تُقرأ من **الإطار المكتمل السابق**، لا من الإطار الجاري؛ هذا يضمن أن
طلقة أطلقها السكربت نفسه تظهر في الإطار التالي بدل أن تُمسح قبل قراءتها.

مثال لرسم وميض فوهة أو tracer من الطلقات، بلا حاجة إلى معرفة هل أصابت:

```lua
function update(dt)
    for i = 1, nf.shot_count() do
        local s = nf.shot(i)
        if s then
            -- s.x/s.y/s.z نقطة الخروج، وs.dx/s.dy/s.dz اتجاه الوحدة
            -- s.pellet/s.pellets للشوزن، وs.projectile للمقذوف
            -- استخدمها لوميض أو خط tracer أو صوت
        end
    end
end
```

`nf.shot` يعمل حتى عندما تُخطئ الطلقة؛ `nf.impact` لا يُنشأ إلا عند الإصابة.

مثال حالة مستمرة:

```lua
local target_id, target_gen = nf.find_entity("Enemy")
if target_id then
    nf.apply_status(target_id, target_gen, "burn", 4, 3, 0.5)
    -- لاحقًا: nf.status_remaining(target_id, target_gen, "burn")
    -- nf.clear_status(target_id, target_gen, "burn")
end
```

الحالة تُطبَّق على الصحة عبر مقاومة نوعها والدرع، وتُنتج إصابة بـ`status=true`
عند كل نبضة. `burn` نار، و`bleed` رصاص، و`corrode` انفجار.

ذاكرة القتال والكبت:

```lua
local attacker_id, attacker_gen = nf.last_attacker(target_id, target_gen)
local health, armor, hits, seconds = nf.damage_taken(target_id, target_gen)
local pressure = nf.suppression(attacker_id, attacker_gen)
```

`nf.last_attacker` يفضّل من أصاب أخيرًا حتى لو اختفى من الإطار؛ `nf.killer` لا
يعود إلا بعد ضربة قاتلة مسجّلة. `nf.suppression` تقرأ مستوى الكبت الحالي للوكيل.

### متوفّر من Lua القياسية
`math.*` (sin, cos, abs, floor, ceil, sqrt, random, pi…)، `string.*`
(format, sub, rep…)، `table.*`، `utf8.*`، `coroutine.*`.

### غير متوفّر (مقصود للأمان)
`io`، `os`، `require`، `dofile`، `loadfile` — لا ملفات، لا شِلّ، لا حِزَم خارجية.

---

### 7.1 لعبة كاملة: اصطياد الصناديق

هذا سكربت `player.lua` كامل: تحرّك الكرة بـ WASD/الأسهم، والاقتراب من
`CrateA` أو `CrateB` يجمعها ويزيد النقاط:

```lua
local speed = 4.0
local score = 0
local targets = { "CrateA", "CrateB" }
local collected = {}
local won = false

function update(dt)
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if not x then return end

    -- منسوبة للكاميرا: الفأرة تدوّر الكاميرا، فـ W يمشي بعيداً عنها.
    -- ومشهد بلا كاميرا يعيد المحورين الخامّين كما كانا.
    local mx, mz = nf.camera_move()
    nf.set_entity_pos(self_id, self_gen,
        x + mx * speed * dt, y, z + mz * speed * dt)

    for _, name in ipairs(targets) do
        local id, gen = nf.find_entity(name)
        if id and not collected[name] then
            local tx, ty, tz = nf.entity_pos(id, gen)
            if tx then
                local dx, dz = tx - x, tz - z
                if dx * dx + dz * dz < 1.5 * 1.5 then
                    -- الجسم الساكن يعيد Transform الخاص به، لذا نخفي الصندوق
                    -- بالحجم ونتذكر أنه جُمع.
                    nf.set_entity_scale(id, gen, 0.001, 0.001, 0.001)
                    collected[name] = true
                    score = score + 1
                    print("score", score)
                end
            end
        end
    end

    if score >= #targets and not won then
        won = true
        print("YOU WIN")
    end
end
```

اربطه بكيان `Player` في `Main.nfscene`:

```
Script: lang=lua path=content://Scripts/player.lua
```

ثم شغّل اللعبة:

```bat
nf run --project Games\CrateHunter\CrateHunter.nfproj
```

---

## 8. أخطاء شائعة (و حلولها)

| العَرَض | السبب | الحل |
|:--|:--|:--|
| السكربت لا يعمل أبدًا | ما فيه `update(dt)` | عرّفها |
| "script lost update(dt); parking" | في خطأ وقت التشغيل أوقف `update` | اقرأ اللوج، صلّح، أعد |
| الكائن لا يتحرك | `entity_pos` ترجع `nil` (بلا تحويل) | تأكد إن الكيان عنده Transform |
| خطأ "not found in VFS" | مسار خاطئ أو مسافة بالمسار | صحّح `path=content://…` |
| رسالة الترحيب تتكرر كل إطار | الشرط مو محفوظ بين الإطارات | حطّ `local logged = false` فوق `update` |

> **قاعدة:** لما يفشل شي، شغّل من الطرفية واقرأ اللوج. المحرّك صادق ومفصّل.

---

## 9. تدريبات لك (نفّذها بالترتيب)

1. **بدّل السرعة:** عدّل `speed` بـ `example.lua`، شوف الفرق.
2. **حرّك أفقياً:** غيّر `x` بدل `y`.
3. **عدّاد إطارات:** اطبع كم إطار مرّ (`clock` يتضاعف كل ثانية).
4. **سقوط:** خلّي كائن ينزل باستمرار (`y - 2 * dt`).
5. **أوّل لعبة:** خلّي كائن يتحرك يمين/يسار ويطبع "وصل للنهاية" عند `x > 5`.

---

> **التالي لما تتقن هون:** الفيزياء والاصطدامات (RigidBody/Collider بالمحرّر)،
> ثم الأصوات والمواد. راجع [`Getting_Started_Ar.md`](Getting_Started_Ar.md)
> للمسار الكامل.
