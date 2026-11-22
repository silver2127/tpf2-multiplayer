local f = assert(io.open(arg[1], "rb"))
local source = f:read("*a"); f:close()
local first = assert(source:find("function CM.chatWrap", 1, true))
local last = assert(source:find("local function readDash", first, true))
local CM = {}
assert(load("return function(CM)\n" .. source:sub(first,last-1) .. "\nend"))()(CM)
local wrapped = CM.chatWrap({string.rep("W", 53)})
assert(#wrapped == 2 and #wrapped[1] == 52 and wrapped[2] == "    W")
for _, text in ipairs({string.rep("W", 300), "Player: " .. string.rep("wide words ", 20), "https://example.invalid/" .. string.rep("longlink",30)}) do
    local lines = CM.chatWrap({text})
    assert(#lines > 1)
    for _, line in ipairs(lines) do assert(#line <= 52) end
    assert(table.concat(lines):gsub("%s", "") == text:gsub("%s", ""))
end
assert(#CM.chatWrap({string.rep("W", 60)},64)==1)
assert(#CM.chatWrap({string.rep("W", 13)},1)==2)
print("chat wrap: default 52-byte width, words, links, preservation and explicit widths passed")
