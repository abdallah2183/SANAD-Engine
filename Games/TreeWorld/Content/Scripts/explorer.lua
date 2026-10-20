-- content://Scripts/explorer.lua
-- TreeWorld — a small third-person forest walk.
--
-- WASD / arrows move the explorer; collect five fireflies. The camera
-- follows the explorer, so this is a third-person game rather than a
-- fixed wide shot.
--
-- The explorer is ONE entity with a rig: a glTF that carries a skeleton
-- and three clips (idle, walk, run). The engine skins it and plays the
-- clip that matches how fast the character is moving -- this script moves
-- the character, and the locomotion does the rest.

local speed = 3.2
local bound = 5.2
local flies = { "Firefly1", "Firefly2", "Firefly3", "Firefly4", "Firefly5" }
local collected = {}
local score = 0
local won = false
local clock = 0
local announced = false

local function close_enough(ax, az, bx, bz, r)
    local dx, az_ = ax - bx, az - bz
    return dx * dx + az_ * az_ < r * r
end

function update(dt)
    if not announced then
        announced = true
        print("TreeWorld: WASD / arrows to walk, collect the five fireflies")
    end

    clock = clock + dt
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if not x then return end

    local mx = nf.axis("move_x")
    local mz = nf.axis("move_z")
    local len = math.sqrt(mx * mx + mz * mz)
    if len > 1.0 then
        mx, mz = mx / len, mz / len
    end
    x = nf.clamp(x + mx * speed * dt, -bound, bound)
    z = nf.clamp(z + mz * speed * dt, -bound, bound)

    -- The character stands on the ground: the rig's origin is at its feet.
    nf.set_entity_pos(self_id, self_gen, x, 0, z)

    -- And it faces where it is going. A character that slides sideways while
    -- its legs swing forward is the most visible thing a third-person game can
    -- get wrong, and the script is the only thing that knows which way the
    -- player pressed.
    if len > 0.05 then
        nf.set_entity_rot(self_id, self_gen, math.deg(math.atan(mx, mz)))
    end

    for _, name in ipairs(flies) do
        if not collected[name] then
            local id, gen = nf.find_entity(name)
            if id then
                local fx, fy, fz = nf.entity_pos(id, gen)
                if fx and close_enough(x, z, fx, fz, 1.25) then
                    nf.set_entity_scale(id, gen, 0.001, 0.001, 0.001)
                    collected[name] = true
                    score = score + 1
                    print("firefly", score, "/", #flies)
                end
            end
        end
    end

    if score >= #flies and not won then
        won = true
        print("ALL FIREFLIES FOUND — the forest is awake")
    end
end
