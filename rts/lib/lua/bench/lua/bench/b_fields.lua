-- OOP objects with 5..40 string fields; method calls via __index class tables;
-- self.x reads/writes (OP_GETTABLE/OP_SETTABLE/OP_SELF with constant string keys)
local Unit = {}
Unit.__index = Unit
local Base = {}
Base.__index = Base
setmetatable(Unit, Base)
function Base:baseValue() return self.hp * 0.5 end
function Unit.new(id)
  local o = setmetatable({ id = id, x = id * 1.5, y = 0, z = id * 0.25, vx = 1, vy = 0, vz = -1,
    hp = 100, maxhp = 120, speed = 2.5, range = 300, reload = 0, team = id % 4, alive = true,
    target = false, name = "unit" .. id, buildProgress = 1, cloaked = false, stunned = false,
    paralyze = 0, experience = 0.1, armor = 1, weaponCount = 2, heading = 0, pitch = 0,
    lastUpdate = 0, cost = 50, metal = 0, energy = 0, radius = 20, height = 30,
    defID = id % 13 + 1, ally = id % 2, transported = false, selfd = false,
    queue = 0, state = 1, moveState = 1, fireState = 2 }, Unit)
  return o
end
function Unit:update(dt)
  local speed = self.speed
  self.x = self.x + self.vx * speed * dt
  self.y = self.y + self.vy * speed * dt
  self.z = self.z + self.vz * speed * dt
  if self.reload > 0 then self.reload = self.reload - dt end
  if self.hp < self.maxhp then self.hp = self.hp + self.armor * dt end
  self.lastUpdate = self.lastUpdate + 1
  return self.hp
end
function Unit:dist2(o)
  local dx, dz = self.x - o.x, self.z - o.z
  return dx * dx + dz * dz
end

local small = {}
for i = 1, 200 do small[i] = { a = i, b = 2, c = 3, d = 4, e = 5 } end
local units = {}
for i = 1, 400 do units[i] = Unit.new(i) end

local acc = 0
for frame = 1, 2500 do
  for i = 1, #units do
    local u = units[i]
    acc = acc + u:update(0.033)
    if u.alive and not u.cloaked and u.team ~= 3 then
      acc = acc + u:dist2(units[(i % #units) + 1]) * 1e-6 + u:baseValue() * 1e-3
    end
  end
  for i = 1, #small do
    local s = small[i]
    s.a = s.a + s.b * s.c - s.d + s.e
    if s.a > 1e6 then s.a = 0 end
  end
end
if not QUIET then print("fields", acc) end
