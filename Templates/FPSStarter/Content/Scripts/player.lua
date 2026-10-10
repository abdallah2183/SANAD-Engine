-- content://Scripts/player.lua
-- ============================================================
--  سلوك اللاعب — قالب «مطلق نار منظور أول» (FPS)
--  مُرفَق بكيان اللاعب (Player). اضغط «تشغيل» وسترى اللاعب يتمايل
--  يمينًا ويسارًا بثبات (مثل التأرجح الجانبي أثناء الجري).
-- ============================================================
--
--  الأفكار التي تحتاجها:
--   1) update(dt) تُنادى كل إطار. dt = الزمن منذ آخر إطار بالثواني.
--   2) self_id / self_gen = معرّف الكيان الذي يحمل هذا السكربت.
--   3) nf.entity_pos(id,gen) -> x,y,z  و  nf.set_entity_pos(id,gen,x,y,z) تحرّكه.
--
--  المتغيّرات (local) فوق update تُحفظ بين الإطارات = ذاكرة السكربت.

local base_x = 0        -- موضع اللاعب الأفقي الأصلي (من المحرّر).
local range  = 1.5      -- مدى التمايل الأفقي.
local speed  = 0.8      -- سرعة التمايل (دورات/ثانية).

local clock  = 0        -- ساعة السكربت: الزمن المتراكم.

function update(dt)
    clock = clock + dt

    -- اقرأ الموضع الحالي أولًا (nil = الكيان بلا تحويل بعد).
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if x == nil then return end

    -- تمايل ناعم حول base_x باستخدام موجة جيبية.
    local new_x = base_x + math.sin(clock * speed * 6.2831853) * range

    nf.set_entity_pos(self_id, self_gen, new_x, y, z)
end
