-- content://Scripts/player.lua
-- ============================================================
--  سلوك اللاعب — قالب «شخص ثالث» (ThirdPerson)
--  هذا الملف مُرفَق بكيان اللاعب (Player). اضغط «تشغيل» وسترى الكرة
--  تتمايل وتدور في الهواء ثم تهبط نحو الأرض.
-- ============================================================
--
--  الأفكار التي تحتاجها:
--   1) update(dt) تُنادى كل إطار. dt = الزمن منذ آخر إطار بالثواني.
--   2) self_id / self_gen = معرّف الكيان الذي يحمل هذا السكربت.
--   3) nf.entity_pos(id,gen) -> x,y,z  و  nf.set_entity_pos(id,gen,x,y,z) تحرّكه.
--
--  المتغيّرات (local) فوق update تُحفظ بين الإطارات = ذاكرة السكربت.

local base_y = 1.2      -- ارتفاع اللاعب الأصلي (من المحرّر).
local range  = 0.6      -- مدى التمايل الرأسي.
local speed  = 1.0      -- سرعة التمايل (دورات/ثانية).

local clock  = 0        -- ساعة السكربت: الزمن المتراكم.

function update(dt)
    clock = clock + dt

    -- اقرأ الموضع الحالي أولًا (nil = الكيان بلا تحويل بعد).
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if x == nil then return end

    -- تمايل ناعم حول base_y باستخدام موجة جيبية.
    local new_y = base_y + math.sin(clock * speed * 6.2831853) * range

    nf.set_entity_pos(self_id, self_gen, x, new_y, z)
end
