"""Exercise the production log prompt across GUI reloads, without uploading logs."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory

from lupa.lua52 import LuaRuntime


MOD = Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/mp'
SOURCE = (MOD / 'desyncreport.lua').read_text(encoding='utf-8')
IO_SOURCE = (MOD / 'io.lua').read_text(encoding='utf-8')   # CM.clearFile: the game's os has no remove


def world(directory, *, desyncs=0, held=False, token='world1'):
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.globals().base = directory.as_posix() + '/'
    lua.globals().world_token = token
    lua.execute('''
        CM={netDir=function() return base end}; K={BASE=base,INSTANCE='b'}
        buttons={}; windows=0
        local function widget(text)
            return {text=text,visible=true,
                setVisible=function(self,v) self.visible=v end,
                addHideOnCloseHandler=function(self) self.close=function() self.visible=false end end,
                onClick=function(self,f) self.click=f; buttons[self.text.text]=self end,
                addItem=function() end,setLayout=function() end,setPosition=function() end}
        end
        api={gui={comp={TextView={new=widget},Button={new=widget},Component={new=widget},
            Window={new=function(t) windows=windows+1; return widget(t) end}},
            layout={BoxLayout={new=widget}}}}
        function tick(n,held)
            CM.desyncReportTick({boot=os.time(),wall=os.time(),t=432,resynctoken=world_token,
                desyncs=n,verdict='DESYNC vs a',resync=held and '1' or nil})
        end
    ''')
    lua.execute('os.remove=nil; os.rename=nil')   # the game's cut-down os table (io.lua, 0.4.11)
    lua.execute(IO_SOURCE)(lua.globals().CM, lua.globals().K, lambda _: None)
    lua.execute(SOURCE)(lua.globals().CM, lua.globals().K, lambda _: None)
    lua.globals().tick(desyncs, held)
    return lua


def kept(path):
    # an emptied file is how the mod removes one (io.lua); readers treat it as absent
    return path.exists() and path.stat().st_size > 0


with TemporaryDirectory() as temporary:
    directory = Path(temporary)
    pending = directory / 'tpf2mp_desync_pending.txt'
    inbox = directory / 'lobby_in.jsonl'
    state = directory / 'lobby_state.json'
    prefs = directory / 'tpf2mp_prefs.txt'
    state.write_text('{"session":"abc123"}', encoding='utf-8')

    first = world(directory, desyncs=1)
    assert kept(pending) and not inbox.exists()
    first.execute('assert(windows==1 and CM.desyncWin.visible); tick(0,true); assert(not CM.desyncWin.visible)')
    reloaded = world(directory, held=True)
    reloaded.execute('assert(windows==0); tick(0,false); assert(windows==1 and CM.desyncWin.visible)')
    assert not inbox.exists(), 'Restoring a question must not grant upload consent'
    reloaded.execute("buttons['  Only this once  '].click()")
    assert not kept(pending)
    commands = [json.loads(line) for line in inbox.read_text(encoding='utf-8').splitlines()]
    assert len(commands) == 1 and commands[0]['cmd'] == 'upload_logs' and commands[0]['t'] == 432
    again = world(directory, desyncs=1)
    again.execute('assert(windows==0)')
    assert len(inbox.read_text(encoding='utf-8').splitlines()) == 1

    # Same lobby, new recovered world: a second incident asks again. The first
    # world's Only this once response is not consent for the second upload.
    recovered = world(directory, token='world2')
    recovered.execute('assert(windows==0); tick(1,false); tick(2,false); assert(windows==1)')
    assert len(inbox.read_text(encoding='utf-8').splitlines()) == 1
    recovered.execute("buttons['  Only this once  '].click(); tick(3,false); assert(windows==1)")
    commands = [json.loads(line) for line in inbox.read_text(encoding='utf-8').splitlines()]
    assert [c['world'] for c in commands] == ['world1', 'world2']
    world(directory, token='world2', desyncs=2).execute('assert(windows==0)')

    # An explicit Never answer is retained, including across a new GUI state.
    state.write_text('{"session":"def456"}', encoding='utf-8')
    declined = world(directory, desyncs=1)
    declined.execute("buttons['  Never  '].click()")
    assert not kept(pending) and 'desync_logs=never' in prefs.read_text(encoding='utf-8')
    world(directory, desyncs=1).execute('assert(windows==0)')

    # Closing the question is not an answer; a failed IPC send is not an answer either.
    prefs.write_text('desync_logs=ask\n', encoding='utf-8')
    state.write_text('{"session":"aaa789"}', encoding='utf-8')
    closed = world(directory, desyncs=1)
    closed.execute('CM.desyncWin.close(); assert(not CM.desyncWin.visible)')
    retried = world(directory)
    retried.execute("CM.netDir=function() return nil end; buttons['  Only this once  '].click(); assert(CM.desyncWin.visible)")
    assert kept(pending)
    retried.execute("CM.netDir=function() return base end; buttons['  Only this once  '].click()")
    assert not kept(pending)

    # A pending question from another lobby must not be resurrected.
    pending.write_text('oldsession\nDESYNC\n432\n1\n', encoding='utf-8')
    world(directory).execute('assert(windows==0)')
    assert not kept(pending)

    # An explicit Always send choice made while paused is honored on return.
    state.write_text('{"session":"bbb789"}', encoding='utf-8')
    world(directory, desyncs=1)
    prefs.write_text('desync_logs=always\n', encoding='utf-8')
    automatic = world(directory, held=True)
    before = inbox.read_text(encoding='utf-8')
    automatic.execute('assert(windows==0); tick(0,false); tick(0,false); assert(windows==0)')
    assert not kept(pending)
    after = inbox.read_text(encoding='utf-8')
    assert len(after.splitlines()) == len(before.splitlines()) + 1
    auto_next = world(directory, desyncs=1, token='anotherWorld')
    auto_next.execute('tick(2,false); assert(windows==0)')
    # upload_logs plus the informational note; only one upload for this world
    sent = [json.loads(line) for line in inbox.read_text().splitlines()]
    assert sum(c.get('cmd') == 'upload_logs' and c.get('world') == 'anotherWorld' for c in sent) == 1

with TemporaryDirectory() as temporary:
    directory = Path(temporary)
    (directory / 'lobby_state.json').write_text('{"session":"sameLobby"}')
    world(directory, desyncs=1, token='oldWorld')
    restored = world(directory, desyncs=1, token='newWorld')
    restored.execute("buttons['  Only this once  '].click(); tick(1,false); assert(windows==2 and CM.desyncWin.visible)")
    cmds = [json.loads(s) for s in (directory/'lobby_in.jsonl').read_text().splitlines()]
    assert len(cmds) == 1 and cmds[0]['world'] == 'oldWorld'
    restored.execute("buttons['  Never  '].click()")
    world(directory, desyncs=1, token='thirdWorld').execute('assert(windows==0)')

# Exercise the real background-worker gate, replacing collection/network only.
# No log content is gathered and no HTTP request is made by this regression.
import sys
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import desynclogs

class ImmediateThread:
    def __init__(self, target, args, **kwargs):
        self.target, self.args = target, args
    def start(self):
        self.target(*self.args)

desynclogs._sent.clear()
with patch.object(desynclogs.threading, 'Thread', ImmediateThread), \
     patch.object(desynclogs.time, 'sleep'), \
     patch.object(desynclogs, 'build_capped', return_value=(b'test', {'files': []})), \
     patch.object(desynclogs, 'post', return_value={'id':'mock-report'}) as post:
    sink, log = Mock(), Mock()
    assert desynclogs.start(sink, log, {'world':'world1'}, 'test')
    assert not desynclogs.start(sink, log, {'world':'world1'}, 'test')
    assert desynclogs.start(sink, log, {'world':'world2'}, 'test')
    assert post.call_count == 2
    post.side_effect = RuntimeError('simulated failure')
    assert desynclogs.start(sink, log, {'world':'world3'}, 'test')
    assert 'world3' not in desynclogs._sent
    post.side_effect = None
    assert desynclogs.start(sink, log, {'world':'world3'}, 'test')
    assert desynclogs.start(sink, log, {}, 'test')
    assert not desynclogs.start(sink, log, {'world':'invalid token'}, 'test')

print('PASS: per-world consent and upload deduplication, second desync after resync, pending old consent, never, failed-send retry; no real uploads')
