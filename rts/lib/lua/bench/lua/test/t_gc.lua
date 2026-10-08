-- GC-observable behaviour: __gc order, weak tables, dead keys in next(), collectgarbage numbers
local order = {}
local function mk(name)
  local u = newproxy(true)
  getmetatable(u).__gc = function() order[#order + 1] = name end
  return u
end
local keep = {}
for i = 1, 30 do local u = mk("u" .. i); if i % 3 == 0 then keep[#keep + 1] = u end end
collectgarbage()
print("gc order", table.concat(order, ","))
order = {}
keep = nil
collectgarbage()
print("gc order 2", table.concat(order, ","))

-- weak values / keys / both
local wv = setmetatable({}, { __mode = "v" })
local wk = setmetatable({}, { __mode = "k" })
local wkv = setmetatable({}, { __mode = "kv" })
local hold = {}
for i = 1, 50 do
  local t = { i }
  wv[i] = t; wv["s" .. i] = t
  wk[t] = i
  wkv[t] = t
  if i % 4 == 0 then hold[#hold + 1] = t end
end
collectgarbage()
local function count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
print("weak", count(wv), count(wk), count(wkv))
local ks = {}
for k, v in pairs(wv) do ks[#ks + 1] = tostring(k) end
print("wv keys", table.concat(ks, ","))

-- dead keys: removing string keys, GC marks them dead, then next() continues past them
local t = {}
for i = 1, 40 do t["key" .. i] = i end
local seen = {}
for k, v in pairs(t) do
  seen[#seen + 1] = k
  t[k] = nil                    -- allowed during traversal
  if #seen % 7 == 0 then collectgarbage() end
end
print("seen", #seen, table.concat(seen, ","))
for i = 1, 40 do t["key" .. i] = i * 2 end   -- re-insert into slots with dead keys
local o = {}
for k, v in pairs(t) do o[#o + 1] = k .. "=" .. v end
print("reinsert", table.concat(o, ","))

-- incremental GC interleaved with table churn; print allocation-dependent counters
collectgarbage("setpause", 100)
collectgarbage("setstepmul", 400)
local big = {}
for r = 1, 50 do
  for i = 1, 200 do big[(r * 37 + i) % 300] = { r, i, "s" .. i } end
  for i = 1, 100 do big[(r * 11 + i) % 300] = nil end
  if r % 10 == 0 then print("count", r, collectgarbage("count")) end
end
print("step", collectgarbage("step", 0), collectgarbage("step", 100))
local ks2 = {}
for k in pairs(big) do ks2[#ks2 + 1] = k end
print("big keys", table.concat(ks2, ","))
-- string interning and collection
local strs = {}
for i = 1, 1000 do strs[i] = string.rep("x", i % 50) .. i end
strs = nil
collectgarbage()
print("final", collectgarbage("count"))
