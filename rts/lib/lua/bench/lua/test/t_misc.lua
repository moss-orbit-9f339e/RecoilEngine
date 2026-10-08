-- coroutines, varargs, tail calls, hooks, debug info, numbers, strings, table library
local p = print
-- coroutines
local co = coroutine.create(function(a, b)
  p("start", a, b)
  local c = coroutine.yield(a + b)
  p("got", c)
  local d, e = coroutine.yield(c * 2)
  p("got2", d, e)
  return "done", 99
end)
p(coroutine.resume(co, 1, 2)); p(coroutine.resume(co, 10)); p(coroutine.resume(co, "x", "y"))
p(coroutine.resume(co), coroutine.status(co))
local gen = coroutine.wrap(function() for i = 1, 5 do coroutine.yield(i, i * i) end end)
for i = 1, 5 do p(gen()) end
p(pcall(coroutine.wrap(function() coroutine.yield(pcall(coroutine.yield, 1)) end)))
local co2 = coroutine.create(function() local t = setmetatable({}, { __index = function(t, k) return coroutine.yield(k) end }); return t.foo end)
p(coroutine.resume(co2))
-- nested coroutines with yields inside loops and C calls
local function producer() return coroutine.create(function() for i = 1, 3 do coroutine.yield(("item%d"):format(i)) end end) end
local pr = producer()
while true do local ok, v = coroutine.resume(pr); if not v then break end p(ok, v) end

-- varargs / select / unpack / multiple returns
local function va(...) return select('#', ...), ... end
p(va()); p(va(nil)); p(va(nil, nil)); p(va(1, nil, 3, nil))
p(select(2, "a", "b", "c")); p(select(-1, "a", "b", "c"))
p(unpack({ 1, 2, nil, 4 }, 1, 4))
local function mr() return 1, 2, 3 end
local t = { mr(), mr() }; p(#t, t[1], t[2], t[4])
p((mr())); p(mr(), "x"); p("x", mr())
local function deep(n, ...) if n == 0 then return ... end return deep(n - 1, n, ...) end
p(deep(10))
-- tail calls and traceback
local function tc(n) if n == 0 then return debug.traceback("tb") end return tc(n - 1) end
p(tc(3))
local function lvl3() return debug.getinfo(1, "nSl").currentline, debug.getinfo(2, "n").name end
local function lvl2() local a, b = lvl3(); return a, b end
p(lvl2())
p(debug.getinfo(print).what, debug.getinfo(1, "S").short_src ~= nil)
do local x, y = 1, "two"; local i = 1; while true do local n, v = debug.getlocal(1, i); if not n then break end; io.write(n, "=", tostring(v), " "); i = i + 1 end; print() end

-- hooks: line, count, call/return (output must match exactly)
local lines = {}
debug.sethook(function(ev, line) lines[#lines + 1] = line end, "l")
local s = 0
for i = 1, 3 do
  s = s + i
end
debug.sethook()
p("lines", table.concat(lines, ","))
local cnt = 0
debug.sethook(function() cnt = cnt + 1 end, "", 3)
local q = {}
for i = 1, 100 do q[i] = i * 2; q["k" .. (i % 5)] = i end
debug.sethook()
p("count hook", cnt)
local evs = {}
local function f1() return 1 end
local function f2() return f1() end
debug.sethook(function(ev) evs[#evs + 1] = ev .. ":" .. tostring(debug.getinfo(2, "n").name) end, "cr")
f2(); math.floor(1.5); local _ = ("x"):upper()
debug.sethook()
p("call hooks", table.concat(evs, ","))

-- numbers (float lua_Number): formatting, coercions, keys
p(1 / 3, 2 / 3, 10 / 4, 1e15, 1e-5, 123456789, 2^24, 2^24 + 1, -0.0, 0 / 0 ~= 0 / 0, 1 / 0, -1 / 0)
p(7 % 3, -7 % 3, 7 % -3, 5.5 % 2, 2^0.5)
p("10" + 5, "3" * "4", 10 .. 20, tonumber("0x10"), tonumber("  12  "), tonumber("1e2"), tonumber("z", 36), tonumber("abc"))
local nk = {}
nk[1] = "int"; nk[1.0] = "float-same"; nk["1"] = "string"; nk[2^24] = "big"; nk[2^24 + 1] = "big+1"; nk[-0.0] = "negzero"; nk[0] = "zero"
p(nk[1], nk["1"], nk[2^24], nk[0], nk[-0.0], #nk)
for i = -3, 3, 0.5 do io.write(i, " ") end print()
for i = 10, 1, -3 do io.write(i, " ") end print()
for i = 1, 0 do p("never") end
-- strings
p(("hello world"):find("o w"), ("abc"):byte(1, -1), string.char(72, 105), ("%d items"):format(3))
p(("a,b,,c"):gsub(",", ";"), ("key=val"):match("(%w+)=(%w+)"), ("  trim  "):match("^%s*(.-)%s*$"))
local parts = {}
for w in ("one two three"):gmatch("%a+") do parts[#parts + 1] = w end
p(table.concat(parts, "|"), ("x"):rep(5, "-") == nil)
p(string.format("%5.1f|%-5d|%05d|%x|%s|%q", 3.14159, 42, 7, 255, "s", "q\"uote"))
p(tostring(1e100), tostring(-1e-100), 255 / 256, 100000000000000)
-- table library
local tl = { 5, 3, 8, 1, 9, 2 }
table.sort(tl); p(table.concat(tl, ","))
table.sort(tl, function(a, b) return a > b end); p(table.concat(tl, ","))
table.insert(tl, 1, 0); table.insert(tl, 100); p(table.remove(tl), table.remove(tl, 1), #tl, table.concat(tl, ","))
p(table.maxn({ 1, 2, [10] = 3 }), #{ 1, 2, nil, 4 }, #{ nil, nil, 3 }, #{ 1, nil }, #{ n = 1 })
local holes = {}
for i = 1, 20 do holes[i] = i end
holes[5] = nil; holes[20] = nil; holes[13] = nil
p(#holes)
for i = 21, 40 do holes[i] = i end
p(#holes)
-- ipairs stops at first nil; pairs order of a mixed table
local mixed = { 1, 2, 3, x = 1, y = 2, [10] = 10, [1.5] = 1.5, [true] = "t" }
for i, v in ipairs(mixed) do io.write(i, ":", v, " ") end print()
for k, v in pairs(mixed) do io.write(tostring(k), "=", tostring(v), " ") end print()
-- closures and upvalues
local function counter() local c = 0; return function() c = c + 1; return c end end
local c1, c2 = counter(), counter()
p(c1(), c1(), c2(), c1())
local fns = {}
for i = 1, 3 do fns[i] = function() return i end end
p(fns[1](), fns[2](), fns[3]())
-- loadstring / string.dump-free paths
p(loadstring("return 1 + ...")(41), pcall(loadstring, "x ="), loadstring("syntax error here"))
