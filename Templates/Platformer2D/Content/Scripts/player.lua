-- content://Scripts/player.lua
-- ============================================================
--  سلوك اللاعب — قالب «منصّات ثنائية الأبعاد» (Platformer2D)
--  مُرفَق بكيان اللاعب (Player). اضغط «تشغيل» وسترى اللاعب يقفز
--  قفزات دورية على المنصّات، ثم يعود لمستواه الأصلي.
-- ============================================================
--
--  الأفكار التي تحتاجها:
--   1) update(dt) تُنادى كل إطار. dt = الزمن منذ آخر إطار بالثواني.
--   2) self_id / self_gen = معرّف الكيان الذي يحمل هذا السكربت.
--   3) nf.entity_pos(id,gen) -> x,y,z  و  nf.set_entity_pos(id,gen,x,y,z) تحرّكه.
--
--  المتغيّرات (local) فوق update تُحفظ بين الإطارات = ذاكرة السكربت.

local base_y  = 1.5     -- مستوى اللاعب الأصلي (من المحرّر).
local hop_max = 1.8     -- أعلى نقطة في القفزة (فوق base_y).
local period  = 1.4     -- مدّة القفزة الواحدة (ثانية).

local clock   = 0       -- ساعة السكربت: الزمن المتراكم.

function update(dt)
    clock = clock + dt

    -- اقرأ الموضع الحالي أولًا (nil = الكيان بلا تحويل بعد).
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if x == nil then return end

    -- موجة جيبية موجبة = قفزة ناعمة: تصعد ثم تهبط باستمرار.
    -- (sin تتراوح بين -1 و1، فنستخدم abs ثم نحوّلها لمدى موجب).
    local t = math.sin(clock / period * 6.2831853)
    local jump = (t + 1) * 0.5 * hop_max   -- من 0 حتى hop_max

    local new_y = base_y + jump

    nf.set_entity_pos(self_id, self_gen, x, new_y, z)
end
