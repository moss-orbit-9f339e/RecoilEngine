-- pairs/next over hash tables (string keys, sparse number keys, table keys)
local strT, numT, objT = {}, {}, {}
for i = 1, 300 do strT["key" .. i] = i end
for i = 1, 3000 do numT[i * 7 + 10000] = i end
local objs = {}
for i = 1, 300 do objs[i] = {}; objT[objs[i]] = i end
local acc = 0
for rep = 1, 2000 do
  for k, v in pairs(strT) do acc = acc + v end
  for k, v in pairs(numT) do acc = acc + v end
  for k, v in pairs(objT) do acc = acc + v end
  local k, v = next(numT)
  while k do acc = acc + 1; k, v = next(numT, k) end
end
if not QUIET then print("pairs", acc) end
