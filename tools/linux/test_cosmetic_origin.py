"""Run the shared capture parser for native cancelled and Windows live edits."""
from pathlib import Path
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[2]
lua = LuaRuntime(unpack_returned_tuples=True)
lua.execute('''
CM={peerSeen=true,vehKeyOf={[7]='a:1'},lineKeyOf={[8]='a:2'},cmMode='coop'}
K={INJECT_FILE='fixture',INSTANCE='a'}
sent={}; logs={}
CM.readFrom=function() return input,0 end
CM.vehKeyFor=function(id) return CM.vehKeyOf[id] end
CM.lineKeyFor=function(id) return CM.lineKeyOf[id] end
CM.scheduleLocal=function(op,args) args.op=op;sent[#sent+1]=args end
''')
lua.execute((root / "mod/mp_lockstep_1/res/scripts/mp/inject.lua").read_text())(
    lua.globals().CM, lua.globals().K, lua.eval("function(s) logs[#logs+1]=s end"))
for entity, kind in [(7, "veh"), (8, "line")]:
    for native in (False, True):
        for op, payload in [("VNAME", "Coal%20Express"), ("VCOLOR", "0.25 0.5 0.75")]:
            lua.globals().input = f"ARMED {int(native)}\n{op} {entity} {payload}" + (" replayOrigin=1" if native else "")
            lua.execute("sent={};CM.pollInject()")
            rows = lua.globals().sent
            assert len(rows) == 1, list(lua.globals().logs.values())
            cmd = rows[1]
            assert cmd.op == op and cmd.kind == kind
            assert cmd.skipOrigin == int(not native)
            if op == "VCOLOR":
                assert cmd.rgb == "0.25,0.5,0.75"
            else:
                assert cmd.name == payload
print("PASS: native names/colors replay origin; Windows captures keep skipOrigin; vehicle and line keys preserved")
