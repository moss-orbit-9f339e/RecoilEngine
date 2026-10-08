-- string building: concatenation, tostring(number), string.format, table.concat
local acc = 0
local fmt = string.format
for i = 1, 400000 do
  local s = "unit_" .. i .. ":" .. (i * 0.5) .. "/" .. "end"
  acc = acc + #s
  local t = fmt("%d %.2f %s", i, i / 3, "x")
  acc = acc + #t
  if i % 100 == 0 then
    local parts = {}
    for j = 1, 50 do parts[j] = tostring(j) end
    acc = acc + #table.concat(parts, ",")
  end
end
if not QUIET then print("concat", acc) end
