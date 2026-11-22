-- Run from the repository root: lua tools/linux/test_gui_identity.lua
-- Execute the shipping dashboard identity/read block and the real mp.io module.
-- Virtual runtime files keep the fixture independent of game APIs and user data.
local function read(path)
    local f = assert(io.open(path, "rb"))
    local s = f:read("*a"); f:close(); return s
end
local source = read("mod/mp_lockstep_1/res/config/game_script/lockstep.lua")
assert(load(source))
local first = assert(source:find("local function readDash(inst)", 1, true))
local last = assert(source:find("local ownWall =", first, true))
local files, reads, fail = {}, 0, false
local base = "/fixture/native/data/"
local identity = base .. "tpf2_instance.txt"
local env = setmetatable({os = {date = os.date}}, {__index = _G})
env.io = {open = function(path, mode)
    assert(mode == "r", "unexpected write")
    if path == identity then
        reads = reads + 1
        if fail then error("identity temporarily unreadable") end
    end
    local body = files[path]
    if body == nil then return nil end
    local nextLine = body:gmatch("([^\n]*)\n")
    return {
        read = function() return nextLine() end,
        lines = function() return nextLine end,
        close = function() end,
        seek = function(_, where) assert(where == "end"); return #body end,
    }
end}
local CM, K = {}, {BASE = base, IDENTITY_FILE = identity}
assert(load(read("mod/mp_lockstep_1/res/scripts/mp/io.lua"), "@mp/io.lua", "t", env))()(CM, K, function() end)
-- Model the game's no-os.remove clearFile fallback without touching the host FS.
CM.clearFile = function(path) files[path] = nil end
local refresh = assert(load("local CM, K = ...\n" .. source:sub(first, last - 1)
    .. "\nreturn own, ownKv", "@dashboard-identity", "t", env))
local function tick() return refresh(CM, K) end
local function setIdentity(letter) files[identity] = letter .. "\npid=123\n" end
-- Missing identity retries each refresh and retains the existing default.
assert(tick() == "a" and K.INSTANCE == nil)
assert(tick() == "a" and reads == 2)
setIdentity("b")
assert(tick() == "b" and K.PROCESS_ID == "123")
files[base .. "lockstep_dash_b.txt"] = "eff=2\ncompany=7\n"
local own, kv = tick()
assert(own == "b" and kv.eff == "2" and reads == 3)
-- The bridge rewrites last session's letter; this refresh must pick it up.
setIdentity("a")
files[base .. "lockstep_dash_a.txt"] = "eff=1\ncompany=3\n"
files[base .. "lockstep_inject_a.txt"] = "existing command\n"
own, kv = tick()
assert(own == "a" and kv.eff == "1" and kv.company == "3")
assert(K.PEER == "b" and K.INJECT_FILE == base .. "lockstep_inject_a.txt")
assert(K.CAPTURE_FILE == base .. "tpf2_capture_a.txt")
assert(K.EVENTS_FILE == base .. "tpf2_events_a.txt")
assert(CM.injectOffset == #"existing command\n" and CM.eventsOffset == -1)
-- Polling an unchanged letter must not reset consumption state or clear peers.
CM.injectOffset, CM.eventsOffset = 123, 456
files[base .. "lockstep_dash_b.txt"] = "eff=2\n"
for _ = 1, 4 do assert(tick() == "a") end
assert(reads == 5 and CM.injectOffset == 123 and CM.eventsOffset == 456)
assert(files[base .. "lockstep_dash_b.txt"])
-- A thrown read is contained by pcall; subsequent periodic polls recover.
fail = true
for _ = 1, 4 do assert(tick() == "a") end
assert(reads == 6)
fail = false
setIdentity("b")
for _ = 1, 3 do assert(tick() == "a") end
assert(tick() == "b" and reads == 7)
assert(K.INJECT_FILE == base .. "lockstep_inject_b.txt")
-- An empty or absent file cannot erase a known identity.
files[identity] = ""
for _ = 1, 4 do assert(tick() == "b") end
files[identity] = nil
for _ = 1, 4 do assert(tick() == "b") end
print("PASS: GUI identity retries, late b->a->b changes, status selection, inject routing, unchanged offsets and read failures")
