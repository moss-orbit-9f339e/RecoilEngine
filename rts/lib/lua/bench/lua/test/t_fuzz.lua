-- Random program fuzzer for table get/set semantics.
-- usage: luadrv t_fuzz.lua <dir> <seed> <nstmts> [gcmode] [hook]
-- Generates Lua source with constant-key and register-key indexing of tables,
-- globals, metatables (__index/__newindex as tables or functions, self-metatables,
-- metatables edited after use), deletions, rawget/rawset, # and next/pairs, GC steps.
-- Everything observable is logged; the log must be identical between VM builds.
local seed = tonumber(arg[2]) or 1
local NSTMT = tonumber(arg[3]) or 300
local GCMODE = tonumber(arg[4]) or 0
local HOOK = tonumber(arg[5]) or 0

local rs = seed % 30000 + 1
local function rnd(n) rs = (rs * 171) % 30269; return rs % n end
local function pick(t) return t[rnd(#t) + 1] end

local ids, nextid = setmetatable({}, { __mode = "k" }), 0
local function repr(v)
  local tv = type(v)
  if tv == "string" then return "s:" .. v
  elseif tv == "number" then
    if v ~= v then return "n:nan" end
    return "n:" .. tostring(v)
  elseif tv == "nil" or tv == "boolean" then return tostring(v)
  else
    local id = ids[v]
    if not id then nextid = nextid + 1; id = nextid; ids[v] = id end
    return tv .. "#" .. id
  end
end
local log, nlog = {}, 0
function out(...)
  local n = select('#', ...)
  local parts = {}
  for i = 1, n do parts[i] = repr((select(i, ...))) end
  nlog = nlog + 1
  log[nlog] = table.concat(parts, " ")
  if nlog >= 2000 then io.write(table.concat(log, "\n"), "\n"); log = {}; nlog = 0 end
end
function dump(t)
  local parts, n = {}, 0
  local k, v = next(t)
  while k ~= nil do
    n = n + 1
    parts[n] = repr(k) .. "=" .. repr(v)
    if n > 300 then break end
    k, v = next(t, k)
  end
  out("dump", t, #t, table.concat(parts, ","))
end

local strkeys = { "a", "b", "c", "x", "y", "z", "hp", "name", "n", "1", "2", "", "__index",
  "__newindex", "__call", "longkey_abcdefghijklmnopqrstuvwxyz_0123456789_ABCDEFGHIJ",
  "longkey_abcdefghijklmnopqrstuvwxyz_0123456789_ABCDEFGHIK", "k1", "k2", "k3", "k4",
  "k5", "k6", "k7", "k8", "k9", "k10", "k11", "k12", "update", "draw" }
local numkeys = { "1", "2", "3", "4", "5", "7", "8", "9", "16", "17", "31", "32", "33", "64",
  "100", "1000", "30000", "-1", "0", "(0/-1)", "1.5", "0.1", "-2.25", "2^24", "(2^24+1)", "2^31",
  "-2^31", "(-2^31-1000)", "1e10", "-1e10", "2^26", "(2^26+1)", "123456", "(1/0)", "(-1/0)" }
local valexprs = { "1", "2", "0", "-1", "3.25", "'str'", "'a'", "true", "false", "nil", "T1", "T2",
  "F1", "{}", "{1,2,3}", "{x=1}", "U1", "(2^24)", "'k1'", "K[1]", "K[2]" }
local NT, NM = 6, 4
local function T() return "T" .. (rnd(NT) + 1) end
local function M() return "M" .. (rnd(NM) + 1) end
local function SK() return pick(strkeys) end
local function NK() return pick(numkeys) end
local function V() return pick(valexprs) end
local function Kreg() return "K[" .. (rnd(12) + 1) .. "]" end
local function isident(s) return s:match("^[%a_][%w_]*$") ~= nil end
local function constidx(t)
  local r = rnd(3)
  if r == 0 then
    local k = SK()
    if isident(k) and k ~= "nil" then return t .. "." .. k end
    return t .. "[" .. string.format("%q", k) .. "]"
  elseif r == 1 then return t .. "[" .. NK() .. "]"
  else return t .. "[" .. Kreg() .. "]" end
end

local gens = {
  function() return constidx(T()) .. " = " .. V() end,
  function() return constidx(T()) .. " = " .. V() end,
  function() return "out(" .. constidx(T()) .. ")" end,
  function() return "out(" .. constidx(T()) .. ")" end,
  function() return "out(" .. constidx(T()) .. ")" end,
  function() return constidx(T()) .. " = nil" end,
  function() local g = "G_" .. pick({ "a", "b", "c", "x", "hp" }); return g .. " = " .. V() end,
  function() return "out(G_" .. pick({ "a", "b", "c", "x", "hp", "missing" }) .. ")" end,
  function() return "out(G_a, G_b, G_missing2)" end,
  function() return "setmetatable(" .. T() .. ", " .. M() .. ")" end,
  function() return "setmetatable(" .. T() .. ", nil)" end,
  function() local t = T(); return "setmetatable(" .. t .. ", " .. t .. ")" end,
  function() return M() .. ".__index = " .. pick({ "T1", "T2", "T3", "IDXF", "nil", "M1", "M2", "STRICT" }) end,
  function() return M() .. ".__newindex = " .. pick({ "T4", "T5", "NIDXF", "nil", "RAWNIDX" }) end,
  function() return "rawset(" .. M() .. ", '__index', " .. pick({ "T1", "T6", "IDXF", "nil" }) .. ")" end,
  function() return "rawset(" .. T() .. ", " .. Kreg() .. ", " .. V() .. ")" end,
  function() return "out(rawget(" .. T() .. ", " .. Kreg() .. "))" end,
  function() return "dump(" .. T() .. ")" end,
  function() return "out(#" .. T() .. ")" end,
  function() return "out(next(" .. T() .. "))" end,
  function() local t = T(); return "for i = 1, " .. (rnd(40) + 1) .. " do " .. t .. "[i] = i end" end,
  function() local t = T(); return "for i = " .. (rnd(40) + 1) .. ", 1, -1 do " .. t .. "[i] = nil end" end,
  function() local t = T(); return "for i = 1, " .. (rnd(20) + 1) .. " do " .. t .. "['k'..i] = i end" end,
  function() local t = T(); return "for i = 1, " .. (rnd(20) + 1) .. " do " .. t .. "['k'..i] = nil end" end,
  function() local t = T(); return "for k in pairs(" .. t .. ") do if type(k)=='string' then " .. t .. "[k] = nil break end end" end,
  function() local t = T(); return "do local s = 0 for k, v in pairs(" .. t .. ") do s = s + 1 end out(s) end" end,
  function() return "collectgarbage('step', " .. (rnd(4) * 10) .. ")" end,
  function() return "K[" .. (rnd(12) + 1) .. "] = " .. pick({ "'a'", "'x'", "1", "2", "17", "1.5", "true", "T1", "T2", "F1", "U1", "'k3'", "-1", "0", "2^31", "(0/0)", "nil", "'longkey_abcdefghijklmnopqrstuvwxyz_0123456789_ABCDEFGHIJ'" }) end,
  function() local t = T(); return "out(" .. t .. ":method(1))" end,
  function() local t = T(); return t .. ".method = " .. pick({ "F1", "F2", "nil" }) end,
  function() return "out(" .. constidx(T()) .. " == " .. constidx(T()) .. ")" end,
  function() return constidx(T()) .. " = " .. constidx(T()) end,
  function() local a, b = T(), T(); return a .. ", " .. b .. " = " .. b .. ", " .. a end,
  function() return T() .. " = {" .. pick({ "", "1,2,3", "x=1,y=2", "1,2,x=3,[10]=4", "[1.5]=1,['a']=2" }) .. "}" end,
  function() return "out(ENVF(" .. pick({ "'a'", "'hp'", "'nope'" }) .. "))" end,
  function() return "out(string.len(" .. pick({ "'abc'", "''" }) .. "), (" .. pick({ "'abc'", "'x'" }) .. "):upper())" end,
}

local src = { "local T1, T2, T3, T4, T5, T6, M1, M2, M3, M4, K, F1, F2, U1, IDXF, NIDXF, RAWNIDX, STRICT, ENVF = ...\n" }
local nblocks = 0
local i = 0
while i < NSTMT do
  nblocks = nblocks + 1
  local nb = rnd(8) + 1
  local body = {}
  for j = 1, nb do body[j] = pick(gens)() end
  i = i + nb
  src[#src + 1] = "do local ok, e = pcall(function()\n" .. table.concat(body, "\n") .. "\nend) if not ok then out('ERR', e) end end\n"
end
-- also a straight-line function (registers, no pcall wrapper) per block for register-based access
local chunk, err = loadstring(table.concat(src), "=fuzz")
if not chunk then print("COMPILE ERROR", err); return end

local F1 = function(self, x) return "F1", x end
local F2 = function(self, x) return "F2", self end
local IDXF = function(t, k) out("idx", t, k); if k == "hp" then return 42 end return nil end
local NIDXF = function(t, k, v) out("nidx", t, k, v); if k ~= "x" then rawset(t, k, v) end end
local RAWNIDX = setmetatable({}, { __newindex = function(t, k, v) out("rawnidx", k) rawset(t, k, v) end })
local STRICT = setmetatable({}, { __index = function(t, k) error("strict: " .. tostring(k), 2) end })
local K = { "a", "x", 1, 2, 17, 1.5, true, "k3", -1, 0, 2^31, "hp" }
local U1 = newproxy(true)
getmetatable(U1).__index = function(u, k) return "U:" .. tostring(k) end
local envT = setmetatable({ hp = 7 }, { __index = _G })
local ENVF = setfenv(function(k) if k == "a" then return a end if k == "hp" then return hp end return nope end, envT)
local Ts = {}
for t = 1, 6 do Ts[t] = { 1, 2, x = t } end
local Ms = {}
for m = 1, 4 do Ms[m] = {} end
if GCMODE == 1 then collectgarbage("setpause", 100); collectgarbage("setstepmul", 1000) end
if GCMODE == 2 then collectgarbage("setpause", 50); collectgarbage("setstepmul", 5000) end
if HOOK == 1 then local n = 0; debug.sethook(function() n = n + 1 end, "", 7) end
if HOOK == 2 then debug.sethook(function(ev, line) out("hook", ev, line) end, "l") end
if HOOK == 3 then debug.sethook(function(ev) out("hook", ev, debug.getinfo(2, "S").what) end, "cr") end
local ok, e = pcall(chunk, Ts[1], Ts[2], Ts[3], Ts[4], Ts[5], Ts[6], Ms[1], Ms[2], Ms[3], Ms[4], K, F1, F2, U1, IDXF, NIDXF, RAWNIDX, STRICT, ENVF)
debug.sethook()
if not ok then out("TOPERR", e) end
for t = 1, 6 do dump(Ts[t]) end
for m = 1, 4 do dump(Ms[m]) end
out("globals", G_a, G_b, G_c, G_x, G_hp)
io.write(table.concat(log, "\n"), "\n")
print("blocks", nblocks, "gc", collectgarbage("count"))
