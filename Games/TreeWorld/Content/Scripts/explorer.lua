-- content://Scripts/explorer.lua
-- TreeWorld ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a small third-person forest walk.
--
-- WASD / arrows move the explorer; collect five fireflies. The camera is a
-- fixed wide shot so the whole clearing stays readable.
--
-- The character is six ordinary scene entities (Player body, Head, two arms,
-- two legs) rather than a parent chain: the loader's hierarchy guard is strict,
-- and a flat group is easier to inspect in the outliner. explorer.lua moves the
-- body and places the other five parts around it every frame.

local speed = 3.2
local bound = 5.2
local flies = { "Firefly1", "Firefly2", "Firefly3", "Firefly4", "Firefly5" }
local collected = {}
local score = 0
local won = false
local clock = 0
local announced = false

local parts = {}
local function part(name)
    if not parts[name] then
        local id, gen = nf.find_entity(name)
        parts[name] = { id = id, gen = gen }
    end
    return parts[name].id, parts[name].gen
end

local function close_enough(ax, az, bx, bz, r)
    local dx, dz = ax - bx, az - bz
    return dx * dx + dz * dz < r * r
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

    -- The body is the root entity (mesh + script). y stays at chest height.
    nf.set_entity_pos(self_id, self_gen, x, 1.05, z)

    -- Layer a compact walk cycle on top. All positions are absolute because the
    -- parts are siblings, not children.
    local bob = math.sin(clock * 8.0) * 0.045
    local swing = math.sin(clock * 8.0) * 0.12
    local head, head_gen = part("Head")
    if head then nf.set_entity_pos(head, head_gen, x, 1.55 + bob, z) end
    local neck, neck_gen = part("Neck")
    if neck then nf.set_entity_pos(neck, neck_gen, x, 1.43 + bob, z) end
    local hair, hair_gen = part("Hair")
    if hair then nf.set_entity_pos(hair, hair_gen, x, 1.64 + bob, z) end
    local arm_l, arm_l_gen = part("ArmL")
    if arm_l then nf.set_entity_pos(arm_l, arm_l_gen, x - 0.30, 1.05, z + swing) end
    local arm_r, arm_r_gen = part("ArmR")
    if arm_r then nf.set_entity_pos(arm_r, arm_r_gen, x + 0.30, 1.05, z - swing) end
    local leg_l, leg_l_gen = part("LegL")
    if leg_l then nf.set_entity_pos(leg_l, leg_l_gen, x - 0.18, 0.35, z - swing) end
    local leg_r, leg_r_gen = part("LegR")
    if leg_r then nf.set_entity_pos(leg_r, leg_r_gen, x + 0.18, 0.35, z + swing) end

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
        print("ALL FIREFLIES FOUND ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the forest is awake")
    end
end
