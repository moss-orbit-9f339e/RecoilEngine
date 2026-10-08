-- array part loops: numeric for, t[i] reads/writes, #t, appends, ipairs, 2D arrays
local N = 2000
local a, b = {}, {}
for i = 1, N do a[i] = i * 0.5; b[i] = (i % 7) end
local grid = {}
for x = 1, 64 do local row = {}; grid[x] = row; for z = 1, 64 do row[z] = (x * z) % 17 end end
local acc = 0
for rep = 1, 1200 do
  for i = 1, N do a[i] = a[i] + b[i] * 0.25 end
  for i = 2, N - 1 do acc = acc + (a[i - 1] + a[i + 1]) * 0.5 - a[i] end
  for _, v in ipairs(b) do acc = acc + v end
  local out = {}
  for i = 1, 500 do out[#out + 1] = i end
  acc = acc + #out
  for x = 2, 63 do
    local row, up, dn = grid[x], grid[x - 1], grid[x + 1]
    for z = 2, 63 do acc = acc + row[z] + up[z] - dn[z] + row[z - 1] end
  end
end
if not QUIET then print("arrays", acc) end
