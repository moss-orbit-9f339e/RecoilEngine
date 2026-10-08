-- Lua->Lua calls (recursion, small helpers, varargs, tail calls) and Lua->C calls
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local function add3(a, b, c) return a + b + c end
local function va(...) local n = select('#', ...); local a, b = ...; return n + (a or 0) + (b or 0) end
local function tail(n, acc) if n == 0 then return acc end return tail(n - 1, acc + 1) end
local function multi() return 1, 2, 3 end
local floor, max, abs = math.floor, math.max, math.abs
local valid = Spring.ValidUnitID
local acc = 0
acc = acc + fib(29)
for i = 1, 1800000 do
  acc = acc + add3(i, 1, 2)
  acc = acc + va(i, 2, 3)
  local x, y, z = multi()
  acc = acc + x + y + z
  acc = acc + floor(i * 0.5) + max(i, 3) + abs(-i)
  if valid(i) then acc = acc + 1 end
end
for i = 1, 2000 do acc = acc + tail(100, 0) end
if not QUIET then print("calls", acc) end
