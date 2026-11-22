-- Executes the exact directory resolver and GUI netDir function from the current integration.
-- Extraction avoids bootstrapping game APIs unrelated to filesystem discovery;
-- no shipping Lua file is edited or substituted.
local sourcePath, expectedData, expectedLocal, expectedHome, expectedXdg, expectedNet =
    arg[1], arg[2], arg[3], arg[4], arg[5], arg[6]
local f = assert(io.open(sourcePath, "rb"))
local source = f:read("*a"); f:close()
assert(os.getenv("LOCALAPPDATA") == expectedLocal, "LOCALAPPDATA mismatch")
assert(os.getenv("TPF2MP_DATADIR") == expectedData, "TPF2MP_DATADIR mismatch")
assert(os.getenv("HOME") == expectedHome, "HOME was changed")
assert(os.getenv("XDG_DATA_HOME") == (expectedXdg ~= "<unset>" and expectedXdg or nil), "XDG_DATA_HOME was changed")
assert(not os.getenv("LD_PRELOAD"), "boot preload was inherited by child")
local stop = assert(source:find("K.IDENTITY_FILE =", 1, true))
local k, cm = assert(load(source:sub(1, stop - 1) .. "\nreturn K, CM", "@" .. sourcePath))()
assert(cm.bootOk and k.BASE == expectedData, "unchanged Lua selected the wrong data directory")
local openStart = assert(source:find("function CM.openFile(path, mode)", 1, true))
local openEnd = assert(source:find("\nend\n", openStart, true))
assert(load("return function(CM)\n" .. source:sub(openStart, openEnd + 4) .. "\nend", "@" .. sourcePath))()(cm)
local start = assert(source:find("function CM.netDir()", 1, true))
local finish = assert(source:find("function CM.chatSend", start, true))
assert(load("return function(CM, K)\n" .. source:sub(start, finish - 1) .. "\nend", "@" .. sourcePath))()(cm, k)
assert(cm.netDir() == expectedNet, "unchanged Lua selected the wrong lobby directory")
print("boot environment: shared Lua data and GUI lobby resolvers passed")
