-- metamethods, fasttm flag caching, environments, __index chains, self-metatables
local log = {}
local function p(...) print(...) end

-- class chains (methods through 1..3 levels of __index tables)
local A = {}; A.__index = A
function A:who() return "A", self.name end
local B = setmetatable({}, A); B.__index = B
function B:who2() return "B", self.name end
local C = setmetatable({}, B); C.__index = C
local o = setmetatable({ name = "o" }, C)
p(o:who()); p(o:who2()); p(o.missing, o.name, rawget(o, "who"))

-- flag caching: metatable lookup misses get cached; later rawset of the metamethod
local mt = {}
local t = setmetatable({}, mt)
p(t.x, t.y)                        -- caches "no __index" in mt->flags
rawset(mt, "__index", function(_, k) return "late:" .. k end)
p(t.x, t.y)
mt.__index = nil
p(t.x)
mt.__index = { x = "tbl" }
p(t.x, t.z)
-- __newindex appear/disappear
t.a = 1
mt.__newindex = function(tt, k, v) rawset(tt, k, v * 10) end
t.b = 2; t.a = 3
p(rawget(t, "a"), rawget(t, "b"))
mt.__newindex = nil
t.c = 4
p(rawget(t, "c"))

-- self metatable (the table is its own metatable): flags are reset by the set itself
local s = {}
setmetatable(s, s)
p(s.foo)
s.__index = function(_, k) return "self:" .. tostring(k) end
p(s.foo, s.__index ~= nil)
s.__newindex = function(tt, k, v) rawset(tt, k, "nw" .. tostring(v)) end
s.q = 1
p(rawget(s, "q"))
s.q = 2        -- existing key: no __newindex
p(rawget(s, "q"))
s.__newindex = nil
s.r = 5
p(rawget(s, "r"))

-- existing key with nil value + __newindex (key slot present, value nil)
local mt2 = {}
local u = setmetatable({}, mt2)
u.k = 1; u.k = nil
mt2.__newindex = function(tt, k, v) p("newindex", k, v) end
u.k = 7
p(rawget(u, "k"))
mt2.__newindex = nil
u.k = 8
p(rawget(u, "k"))

-- __index on non-tables
p(("abc"):upper(), ("x"):rep(3), #"hello")
local smt = getmetatable("")
smt.__index = function(str, k) if k == "len2" then return #str * 2 end return string[k] end
p(("abcd").len2, ("q"):upper())
smt.__index = string
local ud = newproxy(true)
getmetatable(ud).__index = function(_, k) return "ud." .. k end
getmetatable(ud).__newindex = function(_, k, v) p("ud set", k, v) end
p(ud.field); ud.f2 = 3

-- __call, __eq, __lt, __le, __concat, __unm, arithmetic, __len(udata), __tostring
local V = {}
V.__index = V
V.__add = function(a, b) return setmetatable({ v = (type(a) == "table" and a.v or a) + (type(b) == "table" and b.v or b) }, V) end
V.__sub = function(a, b) return "sub" end
V.__mul = function(a, b) return "mul" end
V.__div = function(a, b) return "div" end
V.__mod = function(a, b) return "mod" end
V.__pow = function(a, b) return "pow" end
V.__unm = function(a) return "unm" end
V.__eq = function(a, b) return a.v == b.v end
V.__lt = function(a, b) return a.v < b.v end
V.__le = function(a, b) return a.v <= b.v end
V.__concat = function(a, b) return "cat" end
V.__call = function(self, x) return "called", x end
V.__tostring = function(self) return "V(" .. self.v .. ")" end
local v1, v2 = setmetatable({ v = 1 }, V), setmetatable({ v = 2 }, V)
p(tostring(v1 + v2), tostring(v1 + 5), tostring(3 + v2), v1 - v2, v1 * 2, v1 / 2, v1 % 2, v1 ^ 2, -v1)
p(v1 == v2, v1 == setmetatable({ v = 1 }, V), v1 < v2, v1 <= v2, v2 < v1, v1 .. "x", "x" .. v1, v1(9))
local lenud = newproxy(true); getmetatable(lenud).__len = function() return 42 end
p(#lenud)

-- __index function receiving original table; chain of tables ending in function
local base = setmetatable({}, { __index = function(t, k) return "base:" .. tostring(k) end })
local mid = setmetatable({}, { __index = base })
local top = setmetatable({}, { __index = mid })
p(top.x, top[1], top[true])

-- environments: getfenv/setfenv change what OP_GETGLOBAL/OP_SETGLOBAL see
gx = "global"
local function readg() return gx, gy end
local function writeg(v) gy = v end
p(readg())
local env = setmetatable({ gx = "env" }, { __index = _G })
setfenv(readg, env); setfenv(writeg, env)
writeg("in-env")
p(readg(), rawget(_G, "gy"), rawget(env, "gy"))
local strict = setmetatable({}, { __index = function(_, k) error("undeclared " .. k, 2) end,
  __newindex = function(t, k, v) rawset(t, k, v) end })
local function sf() return undeclared_thing end
setfenv(sf, strict)
p(pcall(sf))
-- metatable on _G (strict mode), as in many game codebases
setmetatable(_G, { __index = function(_, k) return "G:" .. k end, __newindex = function(t, k, v) rawset(t, k, v .. "!") end })
p(missing_global)
newglobal = "v"
p(newglobal)
newglobal = "w"   -- existing now: no __newindex
p(newglobal)
setmetatable(_G, nil)
p(missing_global)

-- numeric-key indexing through __index
local arr = setmetatable({ 10, 20, 30 }, { __index = function(t, k) return k * 100 end })
p(arr[1], arr[3], arr[4], arr[2.5], arr[-1], arr[0], arr[2^31])
-- rawequal / rawget / rawset bypass
p(rawequal(v1, v1), rawequal(v1, setmetatable({ v = 1 }, V)), rawget(top, "x"))

-- metatable protection
local prot = setmetatable({}, { __metatable = "locked" })
p(getmetatable(prot), pcall(setmetatable, prot, {}))

-- __index/__newindex being the same table (proxy)
local store = {}
local proxy = setmetatable({}, { __index = store, __newindex = store })
proxy.a = 1; proxy[1] = 2; proxy[1.5] = 3
p(proxy.a, proxy[1], proxy[1.5], rawget(proxy, "a"), store.a)
for k, v in pairs(store) do io.write(tostring(k), "=", tostring(v), " ") end print()

-- many metatables / __gc and __mode are read via fasttm too
local wk = setmetatable({}, { __mode = "k" })
wk[{}] = 1; wk[proxy] = 2
collectgarbage()
local n = 0 for k in pairs(wk) do n = n + 1 end
p("weak keys left", n)
