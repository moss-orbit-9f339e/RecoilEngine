-- unitID-keyed hash tables (sparse integer keys outside the array part): get/set/remove
local seed = 12345
local function rnd(n) seed = (seed * 171) % 30269; return seed % n end
local units, health, ids = {}, {}, {}
for i = 1, 2000 do
  local id = rnd(30000) + 1
  while units[id] do id = rnd(30000) + 1 end
  ids[i] = id
  units[id] = { id = id, hp = 100 }
  health[id] = 100
end
local acc = 0
for frame = 1, 3000 do
  for i = 1, #ids do
    local id = ids[i]
    local u = units[id]
    if u then
      health[id] = health[id] - 0.5
      acc = acc + u.hp + health[id]
    end
  end
  -- churn: remove and re-add a few ids
  for j = 1, 20 do
    local i = rnd(#ids) + 1
    local id = ids[i]
    units[id] = nil; health[id] = nil
    local nid = rnd(30000) + 1
    while units[nid] do nid = rnd(30000) + 1 end
    ids[i] = nid; units[nid] = { id = nid, hp = 100 }; health[nid] = 100
  end
end
if not QUIET then print("unitid", acc) end
