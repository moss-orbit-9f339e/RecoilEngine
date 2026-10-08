-- comparison opcodes: all type combinations, constants vs registers, NaN, -0, metamethods
local nan, pinf, ninf = 0/0, 1/0, -1/0
local mt = { __eq = function(a, b) return a.v == b.v end, __lt = function(a, b) return a.v < b.v end,
             __le = function(a, b) return a.v <= b.v end }
local mtLtOnly = { __lt = function(a, b) return a.v < b.v end }
local mtOther = { __eq = function() return true end }
local t1, t2, t3 = setmetatable({ v = 1 }, mt), setmetatable({ v = 1 }, mt), setmetatable({ v = 2 }, mt)
local l1, l2 = setmetatable({ v = 1 }, mtLtOnly), setmetatable({ v = 2 }, mtLtOnly)
local o1 = setmetatable({ v = 1 }, mtOther)
local u1, u2 = newproxy(true), newproxy(true)
getmetatable(u1).__eq = function() return true end
getmetatable(u2).__eq = getmetatable(u1).__eq
local u3 = newproxy(u1)
local co = coroutine.create(function() end)
local vals = { nil, false, true, 0, -0, 1, -1, 0.5, 2^24, nan, pinf, ninf, "", "a", "b", "ab", "10", "9",
  {}, t1, t2, t3, o1, l1, l2, print, math.floor, function() end, u1, u2, u3, co }
local NV = 32
local out = {}
for i = 1, NV do
  local a = vals[i]
  local row = {}
  for j = 1, NV do
    local b = vals[j]
    local e = (a == b) and "1" or "0"
    local ne = (a ~= b) and "1" or "0"
    local ok1, lt = pcall(function() return a < b end)
    local ok2, le = pcall(function() return a <= b end)
    local ok3, gt = pcall(function() return a > b end)
    row[j] = e .. ne .. (ok1 and (lt and "T" or "F") or "E") .. (ok2 and (le and "T" or "F") or "E") .. (ok3 and (gt and "T" or "F") or "E")
  end
  out[#out + 1] = i .. ": " .. table.concat(row, " ")
end
print(table.concat(out, "\n"))
-- constant operands (RK constants) in each position
local x = 5
print(x == 5, 5 == x, x ~= 5, x < 10, 10 < x, x <= 5, 5 <= x, x > 4, x >= 6, "a" < "b", x == "5", x == nil, nil == x)
local s = "abc"
print(s == "abc", "abc" == s, s < "abd", s <= "abc", s > "ab", s == nil, s ~= "x")
local n = nil
print(n == nil, n ~= nil, nil == n, n == false, false == n)
-- error messages
print(pcall(function() return {} < {} end))
print(pcall(function() return 1 < "x" end))
print(pcall(function() return "x" <= 1 end))
print(pcall(function() return nil < 1 end))
print(pcall(function() return true <= false end))
print(pcall(function() return t1 < l1 end))
-- loops using comparisons
local c = 0
for i = 1, 1000 do if i % 3 == 0 then c = c + 1 elseif i < 500 then c = c + 2 elseif i <= 700 then c = c - 1 end end
print(c)
local w = 0; while w < 100 do w = w + 7 end print(w)
local r = 0; repeat r = r + 1 until r >= 10 print(r)
