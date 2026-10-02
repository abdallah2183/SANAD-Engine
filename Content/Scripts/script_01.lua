-- content://Scripts/script_01.lua
-- Patrol: walk right until x = 3, then left until x = -3, forever.
-- Attach: Inspector > Script > path = content://Scripts/script_01.lua > Attach.
-- Then press Play (scripts only run in Play mode, never while editing).
function update(dt)
  local id, gen = nf.self()
  local x, y, z = nf.entity_pos(id, gen)
  if x == nil then return end

  local speed = 2.0
  local limit = 3.0
  if patrol_dir == nil then patrol_dir = 1.0 end
  if x >= limit then patrol_dir = -1.0 end
  if x <= -limit then patrol_dir = 1.0 end

  nf.set_entity_pos(id, gen, x + patrol_dir * speed * dt, y, z)
end
