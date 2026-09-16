"""Exercise the production GUI inbox resolver and chat writer with isolated files."""
from pathlib import Path
import tempfile
from lupa.lua52 import LuaRuntime
root=Path(__file__).resolve().parents[1]
s=(root/'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text()
start=s.index('function CM.netDir()');end=s.index('\n\t\t\t\t-- Last n chat lines',start)
with tempfile.TemporaryDirectory() as folder:
    base=Path(folder); release=base/'release';legacy=base/'tpf2mp/netpunch'
    legacy.mkdir(parents=True);(legacy/'lobby_out.jsonl').write_text('')
    lua=LuaRuntime();lua.execute('CM={}; env={}; os.getenv=function(k) return env[k] end')
    lua.globals().env['LOCALAPPDATA']=base.as_posix()
    lua.globals().env['TPF2MP_RELEASE_ROOT']=release.as_posix()
    lua.execute(s[start:end]);cm=lua.globals().CM
    assert cm.netDir()==release.as_posix()+'/netpunch'
    assert not cm.chatSend('test') # no stale-inbox fallback before lobby exists
    (release/'netpunch').mkdir(parents=True)
    assert cm.chatSend('test')
    assert 'test' in (release/'netpunch/lobby_in.jsonl').read_text()
    assert not (legacy/'lobby_in.jsonl').exists()
    lua.globals().env['TPF2MP_RELEASE_ROOT']=None
    assert cm.netDir()==legacy.as_posix()
    cm.netDirCached=False
    (legacy/'lobby_out.jsonl').unlink()
    # Limit legacy candidates to this temp directory by stubbing relative opens.
    lua.execute('realOpen=io.open;io.open=function(p,m) if p:sub(1,9)=="netpunch/" then return nil end return realOpen(p,m) end')
    assert cm.netDir() is None
    (legacy/'lobby_out.jsonl').write_text('')
    assert cm.netDir()==legacy.as_posix()
print('PASS: pinned release chat delivery, no stale fallback, legacy and late lobby startup')
