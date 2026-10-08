-- non-localized global lookups: Spring.X, math.X, plus global variables (OP_GETGLOBAL + OP_GETTABLE)
counter = 0
gameState = { frame = 0, paused = false }
local acc = 0
for i = 1, 3000000 do
  local x, y, z = Spring.GetUnitPosition(i % 3000 + 1)
  if Spring.ValidUnitID(i) then
    acc = acc + math.floor(x) + math.max(y, z) + Spring.GetUnitDefID(i)
  end
  counter = counter + 1
  gameState.frame = gameState.frame + 1
  if gameState.paused then acc = acc - 1 end
end
if not QUIET then print("globals", acc, counter, gameState.frame) end
