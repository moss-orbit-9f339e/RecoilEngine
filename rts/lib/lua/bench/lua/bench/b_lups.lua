-- lups-style particle update + draw loops (structure of lups.lua Draw2 / GameFrame,
-- ParticleClasses/NanoParticles.lua Draw, headers/nanoupdate.lua) against stubbed gl calls
local glMultiTexCoord, glColor, glTexture, glCallList = Spring.Noop, Spring.Noop, Spring.Noop, Spring.Noop
local GetUnitPiecePosDir = Spring.GetUnitPiecePosDir
local thisGameFrame = 0
local NanoParticles = {}
NanoParticles.__index = NanoParticles
local lastTexture = ""
function NanoParticles.new(id, unitID)
  local p = setmetatable({ id = id, unitID = unitID, unitpiece = id % 5, texture = "bitmaps/nano.png",
    pos = { 1, 2, 3 }, targetpos = { 4, 5, 6 }, targetposStart = { 7, 8, 9 }, size = 3, sizeSpread = 1,
    sizeGrowth = 0.1, targetradius = 10, life = 30, reuseLinger = 5, delaySpread = 2, color = { 1, 0.5, 0.2, 0.8 },
    inversed = (id % 2 == 0), urot = 0.3, frame = 0, maxLife = 40, stopframe = 100, dlist = id,
    visible = true, alwaysVisible = false, _dead = false, _staticTarget = 0, _lastupdate = false }, NanoParticles)
  return p
end
function NanoParticles:Update(n)
  if self.reuseLinger and self.frame and self.frame ~= 0 and self.frame + self.reuseLinger >= self.maxLife then
    self.frame = 0
  end
  if (self._staticTarget ~= 1) and (not self._dead) and ((not self._lastupdate) or (thisGameFrame - self._lastupdate >= 1)) then
    self._lastupdate = thisGameFrame
    local pos = self.pos
    pos[1], pos[2], pos[3] = GetUnitPiecePosDir(self.unitID, self.unitpiece)
  end
  self.frame = self.frame + n
end
function NanoParticles:Draw()
  if (lastTexture ~= self.texture) then glTexture(self.texture); lastTexture = self.texture end
  local startPos, endPosNew, endPosOld = self.pos, self.targetpos, self.targetposStart
  if (not self.pos) or (not self.targetpos) or (not self.targetposStart) then self._dead = true; return end
  glMultiTexCoord(0, startPos[1], startPos[2], startPos[3], 1)
  glMultiTexCoord(1, endPosNew[1], endPosNew[2], endPosNew[3], 1)
  glMultiTexCoord(2, endPosOld[1], endPosOld[2], endPosOld[3], 1)
  glMultiTexCoord(6, self.size, self.sizeSpread, self.sizeGrowth, self.targetradius)
  local delaySpread = ((self.life ~= self.reuseLinger) and 1 / (self.life - self.reuseLinger)) or 10000
  glMultiTexCoord(7, self.delaySpread, delaySpread)
  local color = self.color
  glColor(color[1], color[2], color[3], color[4])
  if (self.inversed) then glMultiTexCoord(3, self.urot, self.life - self.reuseLinger - self.frame, self.maxLife - self.reuseLinger, self.stopframe)
  else glMultiTexCoord(3, self.urot, self.frame, self.maxLife - self.reuseLinger, self.stopframe) end
  glCallList(self.dlist)
end
local OtherClass = setmetatable({}, { __index = NanoParticles })
OtherClass.__index = OtherClass

local particles = {}
local FxLayer = { [NanoParticles] = {}, [OtherClass] = {} }
local nid = 0
for u = 1, 150 do
  local unitID = u * 97 % 30000 + 1
  for c, Units in pairs(FxLayer) do
    local list = {}
    Units[unitID] = list
    for k = 1, 4 do
      nid = nid + 1
      local p = NanoParticles.new(nid, unitID)
      if c == OtherClass then setmetatable(p, OtherClass) end
      particles[nid] = p
      list[#list + 1] = p
    end
  end
end
local drawn = 0
for frame = 1, 600 do
  thisGameFrame = frame
  for id, fx in pairs(particles) do fx:Update(1) end
  for partClass, Units in pairs(FxLayer) do
    for unitID, UnitEffects in pairs(Units) do
      for i = 1, #UnitEffects do
        local fx = UnitEffects[i]
        if fx.alwaysVisible or fx.visible then fx:Draw(); drawn = drawn + 1 end
      end
    end
  end
end
if not QUIET then print("lups", drawn, particles[1].frame, particles[7].pos[2]) end
