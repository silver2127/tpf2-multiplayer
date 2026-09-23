-- Exercises the real inject decoder and net codec without a game process.
local root = assert(arg[1])
local function module(name, cm, k)
    assert(loadfile(root .. "/mod/mp_lockstep_1/res/scripts/mp/" .. name .. ".lua"))()(cm, k, function() end)
end
local function up(fn, wanted)
    for i = 1, 100 do
        local name, value = debug.getupvalue(fn, i)
        if not name then break end
        if name == wanted then return value end
    end
    error("missing upvalue " .. wanted)
end
local cm, k = {}, {INSTANCE = "A", INJECT_FILE = "test"}
module("net", cm, k)
local encode = up(cm.scheduleLocal, "encodeCmd")
local decode = up(up(cm.pollEvents, "onLine"), "decodeCmd")
local encoded = encode {op="LCREATE", at=1, origin="A", seq=2, name="007"}
local decoded = assert(decode(encoded))
assert(decoded.name == "007")
local captured
cm.peerSeen = true
cm.unescName = function(s) return s end
cm.scheduleLocal = function(op, args) assert(op == "LCREATE"); captured = args end
cm.readFrom = function() return "ARMED 1\nLCREATEX 0.498039216 0.25 1 180 0 name=Line\n", 1 end
module("inject", cm, k)
cm.pollInject()
assert(captured and captured.armed == 1)
assert(captured.color == "0.498039216,0.25,1")
-- The same Windows record has no Linux-only capture fields.
captured = nil
cm.readFrom = function() return "ARMED 1\nLCREATEX 1 0 0 180 0 name=Old\n", 2 end
cm.pollInject()
assert(captured and captured.name == "Old")
local roundTrip = decode(encode {op="LCREATE", at=1, origin="A", seq=3,
    name=captured.name, color=captured.color, wait=captured.wait, stops=captured.stops, armed=captured.armed})
assert(roundTrip.name == "Old" and roundTrip.armed == 1)

-- The unchanged 0.4.22 name/color reader always skips the origin, even for
-- ARMED 1. Linux must block these captures until it can replay them natively.
cm.lineKeyOf = {[12]="s:12"}; cm.vehKeyOf={}; cm.primedLines={}; cm.primedVeh={}
cm.lineKeyFor=function() return "s:12" end
cm.scheduleLocal=function(op,args) assert(op=="VNAME" or op=="VCOLOR"); captured=args end
for _,record in ipairs({"VNAME 12 Name", "VCOLOR 12 0.25 0.5 0.75"}) do
    captured=nil
    cm.readFrom=function() return "ARMED 1\n" .. record .. "\n",3 end
    cm.pollInject()
    assert(captured and captured.skipOrigin==1)
end
print("slice line wire: unchanged Windows create records and name/color origin-skip boundary passed")

-- Fractional/negative/infinite native waits survive the real Lua 5.2 wire path.
cm.stationGroupPos = function() return 1, 2 end
cm.stationPosInGroup = function() return nil end
cm.scheduleLocal = function(op, args) assert(op == "LCREATE"); captured = args end
cm.readFrom = function()
    return "ARMED 1\nLCREATEX 1 0 0 inf 1 98 100 200 3 -1.25 -inf 0 name=Waits\n", 4
end
captured = nil
cm.pollInject()
assert(captured and captured.wait == math.huge)
assert(captured.stops:find(",100,200,3,-1.25,-inf", 1, true))
local waits = decode(encode {op="LCREATE", at=1, origin="A", seq=4,
    wait=captured.wait, stops=captured.stops, armed=1})
assert(cm.waitNum(waits.wait) == math.huge and waits.stops == captured.stops)
print("slice line wire: fractional/negative/infinite waits passed")
