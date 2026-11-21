"""Exercise the production GUI inbox resolver and chat writer with isolated files."""
from pathlib import Path
import tempfile
from lupa.lua52 import LuaRuntime
root=Path(__file__).resolve().parents[1]
s=(root/'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text()
start=s.index('function CM.netDir()');end=s.index('\n\t\t\t\t-- Last n chat lines',start)
of=s.index('function CM.openFile(path, mode)');open_file=s[of:s.index('\nend\n',of)+5]
with tempfile.TemporaryDirectory() as folder:
    base=Path(folder); legacy=base/'tpf2mp/netpunch'
    lua=LuaRuntime();lua.execute('CM={}; K={}; env={}; os.getenv=function(k) return env[k] end')
    lua.globals().env['LOCALAPPDATA']=base.as_posix()
    lua.globals().K.BASE=(base/'tpf2mp/data').as_posix()+'/'
    lua.execute(open_file);lua.execute(s[start:end]);cm=lua.globals().CM
    # Limit candidates to this temp directory by stubbing relative opens.
    lua.execute('realOpen=io.open;io.open=function(p,m) if p:sub(1,9)=="netpunch/" then return nil end return realOpen(p,m) end')
    assert cm.netDir() is None            # a map can load before the host creates its first lobby
    assert not cm.chatSend('test')        # no stale-inbox fallback before the lobby exists
    legacy.mkdir(parents=True);(legacy/'lobby_out.jsonl').write_text('')
    assert cm.netDir()==legacy.as_posix()
    assert cm.chatSend('test')
    assert 'test' in (legacy/'lobby_in.jsonl').read_text()
print('PASS: per-user lobby folder chat delivery, no stale fallback, late lobby startup')
