-- call/return paths: arg count adjustment, multiple results, stack growth, CallInfo growth,
-- C functions (results, errors, yields), __call, varargs, tail calls, hooks around calls
local p = print
local function f0() end
local function f1(a) return a end
local function f3(a, b, c) return a, b, c end
local function many() return 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 end
p(f0(), f1(), f1(1, 2, 3), f3(1), f3(1, 2, 3, 4, 5))
p(f3(many())); p(many()); p((many())); p(({ many() })[10], select('#', many()))
p(f3(f3(1, 2, 3))); p(f1(many()), f1(f0()))
local t = { f3(1, 2), f3(4, 5, 6) }; p(#t, t[1], t[2], t[3], t[4], t[5])
-- many arguments / many locals (maxstacksize near limits) and deep recursion -> stack reallocation
local src = { "local a1" }
for i = 2, 180 do src[#src + 1] = "local a" .. i .. " = " .. i end
src[#src + 1] = "return function(depth, ...) if depth == 0 then return select('#', ...), a180 end return (select(2, ...)) == nil and 0 or 0, depth end"
local bigframe = loadstring(table.concat(src, "\n"))()
p(bigframe(1, 1, 2, 3))
local function rec(n, ...) if n == 0 then return select('#', ...) end return rec(n - 1, n, ...) end
p(rec(150))
local function rec2(n) if n == 0 then return 0 end local x = rec2(n - 1) return x + 1 end
p(rec2(5000))
local function rec3(n) if n == 0 then return math.floor(1.5) end return (rec3(n - 1)) end
p(rec3(3000))
p(pcall(function() local function inf(n) return 1 + inf(n + 1) end return inf(1) end))
p(pcall(function() local function inf(n) return 1 + select(1, inf(n + 1)) end return inf(1) end))
p(pcall(function() local function inf2() return pcall(inf2) end return inf2() end))
-- C functions with varying results
p(math.max(1, 5, 3), string.byte("abc", 1, 3), select(2, "x", "y", "z"), type(nil))
local a, b, c, d = string.byte("ab", 1, 2); p(a, b, c, d)
p(pcall(math.floor)); p(pcall(string.rep, "x", -1)); p(pcall(error, "e", 0))
p(Spring.GetUnitPosition(7)); p((Spring.GetUnitPosition(7))); p(Spring.GetUnitHealth(3))
local x1, x2, x3, x4, x5, x6, x7 = Spring.GetUnitHealth(3); p(x5, x6, x7)
p(select('#', Spring.Noop()), select('#', Spring.Noop(1, 2, 3)))
p(Spring.SumFields({ x = 1, y = 2, z = 3, health = 4, speed = 5 }))
p(unpack({ 1, 2, 3 })); p(tostring(12), tonumber("0x1F"))
-- many C calls with lots of args (C stack checks, LUA_MINSTACK)
local args = {}
for i = 1, 200 do args[i] = i end
p(math.max(unpack(args)), select('#', unpack(args)))
p(pcall(string.format, string.rep("%d", 150), unpack(args)))
-- __call (non-function callee: generic path) and callable tables in loops
local callable = setmetatable({}, { __call = function(self, a, b) return "called", a, b end })
p(callable(1, 2)); p(pcall(function() local n = nil; n() end)); p(pcall(function() local q = 5; q() end))
for i = 1, 3 do p(callable(i)) end
-- varargs functions (generic path), tail calls into Lua and C
local function va(...) local n = select('#', ...); return n, ... end
p(va()); p(va(nil, nil)); p(va(1, 2, 3))
local function tlua(n) if n == 0 then return "tail-done" end return tlua(n - 1) end
local function tc() return math.floor(2.5) end
local function tva(...) return va(...) end
p(tlua(1000), tc(), tva(1, nil, 3))
-- coroutines: yield from Lua via C (coroutine.yield is a C function), resume inside calls
local co = coroutine.wrap(function(a)
  local s = 0
  for i = 1, 5 do s = s + coroutine.yield(i * a) end
  return "sum", s
end)
p(co(10)); p(co(1)); p(co(2)); p(co(3)); p(co(4)); p(co(5))
local function gen(n) return coroutine.wrap(function() for i = 1, n do coroutine.yield(i, f3(i, i * 2)) end end) end
for a1, a2, a3 in gen(3) do p(a1, a2, a3) end
local deepco = coroutine.create(function() local function r(n) if n == 0 then return coroutine.yield("deep") end return r(n - 1) end return r(500) end)
p(coroutine.resume(deepco)); p(coroutine.resume(deepco, "back"))
-- errors inside called functions restore the call stack
for i = 1, 3 do p(pcall(function() local function inner() error("in" .. i) end inner() end)) end
p(pcall(function() return f1() .. "x" end))
-- hooks enabled/disabled from inside calls
local evs = {}
local function hk(ev) evs[#evs + 1] = ev end
local function withhook() debug.sethook(hk, "cr"); local r = f3(1, 2, 3); debug.sethook(); return r end
p(withhook(), table.concat(evs, ","))
evs = {}
local function setret() debug.sethook(hk, "r") end
setret(); f0(); math.floor(1); debug.sethook()
p(table.concat(evs, ","))
evs = {}
debug.sethook(function(ev) evs[#evs + 1] = ev .. ":" .. tostring(debug.getinfo(2, "S").what) end, "crl")
local r = rec(3)
debug.sethook()
p(r, #evs, table.concat(evs, ","))
-- count hook firing inside call-heavy loop
local cnt = 0
debug.sethook(function() cnt = cnt + 1 end, "", 1)
for i = 1, 50 do f3(i, i, i); math.abs(-i) end
debug.sethook()
p("count", cnt)
-- debug.getinfo / traceback from within nested calls
local function lvl() return debug.traceback("tb", 1) end
local function mid() local s = lvl() return s end
p(mid())
p(debug.getinfo(1, "l").currentline, debug.getinfo(f3, "u").nups)
-- generic for with Lua iterators, closures, callable tables, invalid iterators, errors
local function range(n) local i = 0 return function() i = i + 1 if i <= n then return i, i * i end end end
local acc = {}
for a, b in range(4) do acc[#acc + 1] = a .. ":" .. b end
p(table.concat(acc, ","))
local function iter(state, ctl) if ctl < state then return ctl + 1, "v" .. ctl end end
for i, v in iter, 3, 0 do io.write(i, v, " ") end print()
local citer = setmetatable({}, { __call = function(self, s, c) if c < 3 then return c + 1 end end })
for i in citer, nil, 0 do io.write("c", i, " ") end print()
p(pcall(function() for x in nil do end end))
p(pcall(function() for x in 5 do end end))
p(pcall(function() for x in range(3) do if x == 2 then error("iter body " .. x) end end end))
p(pcall(function() for x in function() error("in iterator") end do end end))
local co = coroutine.wrap(function() for x in function(s, c) c = (c or 0) + 1; if c > 3 then return nil end; coroutine.yield(c); return c end do end return "co done" end)
p(pcall(co))
p(pcall(coroutine.wrap(function() for k in next, { 1, 2 } do coroutine.yield(k) end end)))
-- C stack exhaustion while generic-for iterators are being called
for _, mode in ipairs({ "pairs", "luaiter", "ipairs" }) do
  local depth, firstErr = 0, nil
  local function rec(n)
    depth = n
    if mode == "pairs" then for k in pairs({ 1 }) do end
    elseif mode == "ipairs" then for k in ipairs({ 1 }) do end
    else for k in range(1) do end end
    local ok, e = pcall(rec, n + 1)
    if not ok and not firstErr then firstErr = e end
    return true
  end
  rec(1)
  p(mode, firstErr, depth)
end
local function rec2(n) local ok, e = pcall(function() for k, v in pairs({ a = 1 }) do return rec2(n + 1) end end) return ok, e, n end
p(rec2(1))
