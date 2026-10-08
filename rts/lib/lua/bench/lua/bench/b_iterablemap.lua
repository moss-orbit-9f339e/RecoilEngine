-- Zero-K's real IterableMap.lua (LuaRules/Gadgets/Include) driven like the gadgets do
local dir = (arg and arg[1]) or "."
local IterableMap = dofile(dir .. "/IterableMap.lua")
local seed = 4242
local function rnd(n) seed = (seed * 171) % 30269; return seed % n end
local map = IterableMap.New()
for i = 1, 1500 do
  local id = rnd(30000) + 1
  IterableMap.Add(map, id, { unitID = id, timer = rnd(30), hp = 100, x = id, z = -id })
end
local acc = 0
local function tick(unitID, data, index, dt)
  data.timer = data.timer - dt
  acc = acc + data.hp * 0.01 + index * 1e-6
  if data.timer < 0 then
    data.timer = 30
    if data.hp < 1 then return true end
    data.hp = data.hp - 1
  end
end
for frame = 1, 1200 do
  IterableMap.Apply(map, tick, 1)
  for j = 1, 10 do
    local id = rnd(30000) + 1
    if IterableMap.InMap(map, id) then
      local d = IterableMap.Get(map, id)
      acc = acc + d.x
      IterableMap.Remove(map, id)
    else
      IterableMap.Add(map, id, { unitID = id, timer = rnd(30), hp = 100, x = id, z = -id })
    end
  end
  for j = 1, 50 do
    local k, d = IterableMap.Next(map)
    if k then acc = acc + d.z * 1e-3 end
  end
end
if not QUIET then print("iterablemap", acc, IterableMap.GetIndexMax(map)) end
