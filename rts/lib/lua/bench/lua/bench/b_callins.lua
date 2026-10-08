-- gadget-handler style call-in dispatch + unit_script style coroutines (Sleep/WakeUp)
local gadgets = {}
for i = 1, 60 do
  local g = { name = "gadget" .. i, counter = 0, enabled = (i % 5 ~= 0) }
  function g:GameFrame(n) self.counter = self.counter + n % 3 end
  function g:UnitDamaged(unitID, unitDefID, team, damage) if damage > 50 then self.counter = self.counter + 1 end end
  gadgets[#gadgets + 1] = g
end
local callInLists = { GameFrame = {}, UnitDamaged = {} }
for _, g in ipairs(gadgets) do
  if g.enabled then
    for name, list in pairs(callInLists) do if g[name] then list[#list + 1] = g end end
  end
end
local function GameFrame(n)
  local list = callInLists.GameFrame
  for i = 1, #list do local g = list[i]; g:GameFrame(n) end
end
local function UnitDamaged(unitID, unitDefID, team, damage)
  local list = callInLists.UnitDamaged
  for i = 1, #list do local g = list[i]; g:UnitDamaged(unitID, unitDefID, team, damage) end
end
local co_create, co_resume, co_yield = coroutine.create, coroutine.resume, coroutine.yield
local threads = {}
for i = 1, 200 do
  threads[i] = co_create(function()
    local k = 0
    while true do k = k + 1; co_yield(k) end
  end)
end
local acc = 0
for frame = 1, 9000 do
  GameFrame(frame)
  for d = 1, 20 do UnitDamaged(d, d % 13, d % 4, (d * frame) % 100) end
  for i = 1, #threads, 3 do local ok, k = co_resume(threads[i]); acc = acc + k end
end
for _, g in ipairs(gadgets) do acc = acc + g.counter end
if not QUIET then print("callins", acc) end
