-- micro: hash-part number keys and table keys in small (cache-resident) tables
local t, o, objs = {}, {}, {}
for i = 1, 50 do t[i * 1000 + 7] = i end
for i = 1, 50 do objs[i] = {}; o[objs[i]] = i end
local s = 0
for r = 1, 300000 do
  for i = 1, 50 do s = s + t[i * 1000 + 7] + o[objs[i]] end
end
if not QUIET then print("numkeys", s) end
