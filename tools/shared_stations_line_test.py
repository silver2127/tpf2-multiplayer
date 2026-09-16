"""A line whose stops belong to TWO companies, through the real Lua, on Lua 5.2.

The native patch (slice_hook.cpp "SHARED STATIONS") lets company B click company
A's station in the line editor. That is only half the job: the mod's own
replication has to carry such a line across the wire and back without quietly
repairing it to one owner. This loads the real lines.lua, companies.lua and
shared_infra.lua into a lupa.lua52 runtime and drives:

  - lineSnapshot on a line whose stop 1 is A's station group and stop 2 is B's:
    both stops travel, by POSITION, with no owner filtering anywhere;
  - execLine LUPDATE replaying that string on the other peer: buildLineObject
    resolves BOTH groups (A's by position like any other) and the whole stop
    list reaches api.cmd.make.updateLine -- nothing strips the foreign stop;
  - cmReassignEntity(line, B): the line and its vehicles move to B, and A's
    station group, A's station and A's track keep their owner (the scoped
    setter, not the stock setPlayer that would take the stops with it);
  - cmRepairLineOwners: a B line with B vehicles standing at A's station is
    left alone -- it re-owns to the VEHICLES' company, never to a stop's;
  - the refusals stay: cmMayModify is still false for B on A's station, so
    "use" is opened and "edit/demolish" is not.

    python tools/shared_stations_line_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    g = L.globals()
    g.package.path = (os.path.join(REPO, "mod/mp_lockstep_1/res/scripts/?.lua").replace("\\", "/")
                      + ";" + g.package.path)
    g.LINES_SRC = open(os.path.join(MP, "lines.lua"), encoding="utf-8").read()
    g.COMPANIES_SRC = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
    return L.execute(r'''
local logs, sent = {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end

-- THE WORLD, as it stands on company B's machine. Company A is player 1001,
-- company B is player 2002 and is us (api.engine.util.getPlayer). A's station
-- group 5000 (station 5001) sits at 100,200 and B's group 6000 (station 6001)
-- at 400,500; track edge 21 is A's. Line 42 is A's line replayed HERE, so it
-- and its vehicles 30 and 31 have just landed on our local player and are
-- waiting to be handed to A; it stops at A's station and at B's. Vehicle 32 is
-- B's own, on another line, and must not move.
local owners = { [5000] = 1001, [5001] = 1001, [21] = 1001,
                 [6000] = 2002, [6001] = 2002,
                 [42] = 2002, [30] = 2002, [31] = 2002, [32] = 2002 }
local pos = { [5000] = {100, 200}, [5001] = {101.5, 202.5},
              [6000] = {400, 500}, [6001] = {401.5, 502.5} }
local groupStations = { [5000] = {5001}, [6000] = {6001} }
local vehicles = { [30] = {line = 42}, [31] = {line = 42}, [32] = {line = 77} }
local lineStops = {
  { stationGroup = 5000, station = 0, terminal = 0, loadMode = 0,
    minWaitingTime = 0, maxWaitingTime = 180, waypoints = {}, alternativeTerminals = {} },
  { stationGroup = 6000, station = 0, terminal = 1, loadMode = 0,
    minWaitingTime = 0, maxWaitingTime = 180, waypoints = {}, alternativeTerminals = {} },
}
local CT = { LINE = 1, TRANSPORT_VEHICLE = 2, BASE_EDGE = 3, STATION_GROUP = 4,
             PLAYER_OWNED = 5, COLOR = 6, MODEL_INSTANCE_LIST = 7 }

api = setmetatable({}, { __index = function() return sink() end })
api.type = setmetatable({
  ComponentType = CT,
  Line = { new = function() return { stops = {} } end, Stop = { new = function() return {} end } },
  StationTerminal = { new = function() return {} end },
  Vec3f = { new = function(r, g, b) return { r, g, b } end },
  enum = { TransportVehicleState = { IN_DEPOT = 0 } },
}, { __index = function() return sink() end })
api.engine = setmetatable({
  entityExists = function(id) return owners[id] ~= nil end,
  getComponent = function(id, k)
    if k == CT.LINE and id == 42 then return { stops = lineStops, waitingTime = 180 } end
    if k == CT.TRANSPORT_VEHICLE then return vehicles[id] end
    if k == CT.STATION_GROUP then return { stations = groupStations[id] } end
    if k == CT.PLAYER_OWNED and owners[id] then return { player = owners[id] } end
    if k == CT.BASE_EDGE and id == 21 then return { node0 = 100, node1 = 101 } end
    return nil
  end,
  util = { getPlayer = function() return 2002 end },
  system = {
    lineSystem = { getLines = function() return { 42 } end },
    transportVehicleSystem = { getVehiclesWithState = function() return { 31, 32 } end },
    streetSystem = { getNode2TrackEdgeMap = function() return { [100] = { 21 } } end,
                     getNode2StreetEdgeMap = function() return {} end },
  },
}, { __index = function() return sink() end })
api.cmd = {
  make = {
    updateLine = function(lid, line)
      local out = { what = "updateLine", lid = lid, stops = {} }
      for i = 1, #line.stops do out.stops[i] = line.stops[i] end
      return out
    end,
  },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then cb(nil, true) end end,
}

local setPlayerCalls = {}
game = setmetatable({ interface = {
  getName = function() return "B Express" end,
  getEntity = function(id)
    if pos[id] then return { position = pos[id] } end
    return { balance = 0 }
  end,
  getEntities = function(_, opts)
    if opts and opts.type == "STATION_GROUP" then return { 5000, 6000 } end
    if opts and opts.type == "VEHICLE" then return { 30, 32 } end
    return {}
  end,
  setPlayer = function(id, pid)
    setPlayerCalls[#setPlayerCalls + 1] = { id = id, pid = pid }
    -- The engine's own setPlayer(line) ALSO takes the line's stop stations,
    -- their groups and its track edges. Model that, so a test that stops using
    -- the scoped setter starts failing here.
    if pid >= 1610612736 then owners[id] = pid - 1610612736; return end
    owners[id] = pid
    if id == 42 then owners[5000] = pid; owners[5001] = pid; owners[6000] = pid
                     owners[6001] = pid; owners[21] = pid
                     owners[30] = pid; owners[31] = pid end
  end,
  setMaximumLoan = function() end,
  addPlayer = function() return 1001 end,
} }, { __index = function() return sink() end })

-- The identity file the shared-infra capability check reads.
local realOpen = io.open
io.open = function(path, mode)
  if tostring(path):find("identity") then
    return { read = function() return "a\npid=123\nentity_owner_v1=1\n" end, close = function() end }
  end
  if tostring(mode or ""):find("w") or tostring(mode or ""):find("a") then
    return { write = function() end, close = function() end }
  end
  return nil
end

local K = setmetatable({ INSTANCE = "b", PEER = "a", BASE = "", IDENTITY_FILE = "identity",
                         VLINE_RETRY_STEPS = 5, JOURNAL_TRANSFER = 6,
                         STRICT_OPS = { LCREATE = true, LUPDATE = true, LDELETE = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, ticks = 0, seqNo = 0, queue = {}, peers = {} }
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal() end
function CM.escName(s) return tostring(s or ""):gsub(" ", "%%20") end
function CM.unescName(s)
  return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end
local log = function(s) logs[#logs + 1] = s end
assert(load(COMPANIES_SRC, "@companies.lua"))()(CM, K, log)
assert(load(LINES_SRC, "@lines.lua"))()(CM, K, log)

-- companies mode, no cfg file to read: set it up the way cmEnsure would
CM.cmMode = "companies"; CM.cmMyCompany = 2; CM.cmRoster = { 1, 2 }
CM.cmCompanyPid = { [1] = 1001, [2] = 2002 }
CM.cmReady = true; CM.cmLive = true
function CM.cmEnsure() end
function CM.cmEnsurePlayers() end
CM.cmKeyOf = {}
function CM.lineKeyFor(id) if id == 42 then return "b:1" end end
function CM.lineIdFor(key) if key == "b:1" then return 42 end end

local H = { CM = CM }
function H.snapStops() local s = CM.lineSnapshot(42); return s and s.stops end
function H.snapName() local s = CM.lineSnapshot(42); return s and s.name end
function H.replay(stops)
  sent = {}
  CM.execLine({ op = "LUPDATE", origin = "a", seq = 5, at = 100, key = "b:1",
                name = "B%20Express", color = "0.1,0.2,0.3", wait = 180,
                stops = stops, alts = "", armed = 0 })
end
function H.nsent() return #sent end
function H.sentStopCount() local c = sent[1]; return c and c.stops and #c.stops or 0 end
function H.sentStopGroup(i) local c = sent[1]; return c and c.stops[i] and c.stops[i].stationGroup end
function H.sentStopTerminal(i) local c = sent[1]; return c and c.stops[i] and c.stops[i].terminal end
function H.owner(id) return owners[id] end
function H.setOwner(id, pid) owners[id] = pid end
function H.reassign(cid) CM.cmReassignEntity(42, cid, "line") end
function H.repairLines() CM.cmRepairLineOwners("test") end
function H.mayModify(id) return CM.cmMayModify(id) end
function H.nSetPlayer() return #setPlayerCalls end
function H.setPlayerEncoded(i)
  local c = setPlayerCalls[i]; return c and c.pid >= 1610612736
end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def main():
    H = runtime()

    # 1. The snapshot carries BOTH stops, by position, with no owner filtering.
    stops = H.snapStops()
    check("snapshot: two stops travel", stops is not None and stops.count(";") == 1, str(stops))
    check("snapshot: A's station group is in the wire string by position",
          stops is not None and stops.startswith("100.00,200.00,"), str(stops))
    check("snapshot: B's own station follows", stops is not None and ";400.00,500.00," in stops, str(stops))
    check("snapshot: the station's own position rides along (fields 8-9)",
          stops is not None and "101.5,202.5" in stops and "401.5,502.5" in stops, str(stops))
    check("snapshot: the name is read back", H.snapName() == "B%20Express", str(H.snapName()))

    # 2. Replaying it resolves A's group like any other and keeps both stops.
    H.replay(stops)
    check("replay: one updateLine sent", H.nsent() == 1)
    check("replay: both stops reach the command", H.sentStopCount() == 2, str(H.sentStopCount()))
    check("replay: stop 1 is A's station group 5000", H.sentStopGroup(1) == 5000, str(H.sentStopGroup(1)))
    check("replay: stop 2 is B's station group 6000", H.sentStopGroup(2) == 6000, str(H.sentStopGroup(2)))
    check("replay: the terminal choice survives", H.sentStopTerminal(2) == 1, str(H.sentStopTerminal(2)))
    check("replay: no refusal logged", "refused" not in H.logs().lower(), H.logs()[-300:])

    # 3. The handover the replay ends with. This machine is B, so a replayed
    # line built by our local player has to be handed to the company that made
    # it (A) -- and that must not take B's own station 6000 with it, which is
    # exactly what the stock setPlayer(line) would do (the stub models that).
    before = H.nSetPlayer()
    H.reassign(1)
    check("handover: the line went to the company that made it", H.owner(42) == 1001, str(H.owner(42)))
    check("handover: its fleet went with it", H.owner(30) == 1001 and H.owner(31) == 1001,
          f"{H.owner(30)},{H.owner(31)}")
    check("handover: the OTHER company keeps its station group", H.owner(6000) == 2002, str(H.owner(6000)))
    check("handover: the other company keeps its station", H.owner(6001) == 2002, str(H.owner(6001)))
    check("handover: a vehicle on another line is untouched", H.owner(32) == 2002, str(H.owner(32)))
    check("handover: every setPlayer used the scoped, entity-only encoding",
          H.nSetPlayer() > before and all(H.setPlayerEncoded(i)
                                          for i in range(before + 1, H.nSetPlayer() + 1)),
          f"{before} -> {H.nSetPlayer()}")

    # 4. cmRepairLineOwners re-owns to the VEHICLES' company, never to a stop's.
    H.repairLines()
    check("repair: the line stays with its vehicles' company", H.owner(42) == 1001, str(H.owner(42)))
    check("repair: the foreign stop's owner is untouched", H.owner(6000) == 2002, str(H.owner(6000)))
    # ...and it still does its job: a line whose owner disagrees with its fleet.
    H.setOwner(42, 2002)
    H.repairLines()
    check("repair: a line owned against its whole fleet is still corrected",
          H.owner(42) == 1001, str(H.owner(42)))
    check("repair: correcting it did not steal the foreign stop",
          H.owner(6000) == 2002 and H.owner(6001) == 2002, str(H.owner(6000)))

    # 5. USE is opened; EDIT and DEMOLISH are not.
    check("refusal: B still may not change A's station group", H.mayModify(5000) is False)
    check("refusal: B still may not change A's track", H.mayModify(21) is False)
    check("permission: B may still change its own station", H.mayModify(6000) is True)

    print()
    if fails:
        print(f"{len(fails)} FAILED: " + ", ".join(fails))
        return 1
    print("PASS: a line may hold another company's station -- the snapshot ships it, the replay "
          "rebuilds it, the handover leaves its owner alone, the line-owner repair follows the "
          "vehicles, and edits/demolition stay refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
