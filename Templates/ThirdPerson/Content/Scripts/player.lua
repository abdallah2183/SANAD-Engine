-- content://Scripts/player.lua
-- ============================================================
--  Crate Hunter — the playable starter game.
--
--  WASD / arrows move the blue sphere. Touch an orange crate to collect it.
--  Collect both crates to win. Scores are printed to the editor/player log,
--  which is the honest HUD this demo ships with.
--
--  The four engine calls this game is built from:
--    nf.camera_move()            -> x, z of the held input, rotated by the
--                                   camera so W always walks away from it
--    nf.entity_pos(id, gen)      -> x, y, z (nil when no Transform)
--    nf.find_entity("CrateA")    -> id, gen (nil when absent)
--    nf.set_entity_pos(...)      -> move the player / hide a collected crate
-- ============================================================

local speed = 4.0
local score = 0
local targets = { "CrateA", "CrateB" }
local collected = {}
local won = false

function update(dt)
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if not x then return end

    -- Camera-relative: the mouse turns the camera, so "forward" is wherever
    -- the player is looking. In a scene with no camera this returns the raw
    -- axes, which is what the starter did before 0.4.0.
    local mx, mz = nf.camera_move()
    nf.set_entity_pos(self_id, self_gen,
        x + mx * speed * dt,
        y,
        z + mz * speed * dt)

    for _, name in ipairs(targets) do
        local id, gen = nf.find_entity(name)
        if id and not collected[name] then
            local tx, ty, tz = nf.entity_pos(id, gen)
            if tx then
                local dx, dz = tx - x, tz - z
                if dx * dx + dz * dz < 1.5 * 1.5 then
                    -- Static crate bodies snap their Transform back from the
                    -- physics step, so moving one does not hide it. A zero
                    -- scale is a Transform write the body does not own.
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
