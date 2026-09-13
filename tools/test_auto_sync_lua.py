"""Production Lua control pump with real files; engine calls are simulated."""
from pathlib import Path
from tempfile import TemporaryDirectory
import time
from lupa.lua52 import LuaRuntime

source = (Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/mp/resync.lua').read_text(encoding='utf-8')
lua = LuaRuntime(unpack_returned_tuples=True)
lua.execute('assert(load(...))', source)  # includes the chunk's local-variable limit
with TemporaryDirectory() as temporary:
    directory = Path(temporary)
    lua.globals().base = str(directory).replace('\\', '/') + '/'
    lua.execute('''
        CM={resyncToken='test'}; K={BASE=base,PROCESS_ID='123',INSTANCE='a'}
        speed=0; changes=0; hashes=0; paused=false; didInitialUnpause=false
        game={interface={getGameSpeed=function() return speed end}}
        CM.setSpeed=function(v) speed=v; changes=changes+1 end
        CM.recoveryWorldHash=function(t) hashes=hashes+1; return 'hash','fresh:'..t end
    ''')
    lua.execute('CM.recoveryReleasePacing=function() didInitialUnpause=true end')
    lua.execute(source)(lua.globals().CM, lua.globals().K, lambda _:None)
    def control(revision, phase, pid='123', **fields):
        values = dict(pid=pid, operation='a'*32, epoch='b'*32,
                      revision=revision, phase=phase, resume_speed=0, **fields)
        (directory / 'tpf2_sync_lua.txt').write_text(''.join(f'{k}={v}\n' for k,v in values.items()))
    control(1, 'complete')
    lua.execute('assert(not CM.autoSyncPump(100)); assert(changes==0)')  # old completion in a new world
    control(1, 'holding', pid='old')
    lua.execute('assert(not CM.autoSyncPump(100)); assert(changes==0)')
    control(1, 'holding')
    lua.execute('assert(CM.autoSyncPump(100)); assert(changes==1 and CM.resyncHold)')
    lua.execute('assert(CM.autoSyncPump(100)); assert(changes==1)')
    (directory / 'tpf2_sync_lua.txt').unlink()
    lua.execute('assert(CM.autoSyncPump(100)); assert(changes==1)')
    control(2, 'checking')
    lua.execute('assert(CM.autoSyncPump(100)); assert(hashes==1); CM.autoSyncPump(100); assert(hashes==1)')
    ack = (directory / 'tpf2_sync_lua_ack.txt').read_text()
    assert 'fingerprint=hash:fresh:100' in ack and 'paused=1' in ack
    control(3, 'checking')
    lua.execute('CM.autoSyncPump(100); assert(hashes==2)')  # same paused timestamp, fresh revision
    control(4, 'error')
    lua.execute('assert(CM.autoSyncPump(100)); assert(speed==0)')
    control(2, 'complete')  # stale release must not run
    lua.execute('assert(CM.autoSyncPump(100)); assert(changes==1)')
    control(5, 'complete')
    path = directory / 'tpf2_sync_lua.txt'
    path.write_text(path.read_text().replace('resume_speed=0', 'resume_speed=3'))
    lua.execute('assert(CM.autoSyncPump(100)); assert(not didInitialUnpause)')
    path.write_text('pid=123\nphase=complete')
    lua.execute('assert(CM.autoSyncPump(100))')
    # Stale IPC cannot release the GUI producer gate either.
    control(6, 'loading')
    lua.execute('assert(CM.recoveryGuiHeld())')
    control(5, 'complete')
    lua.execute('assert(CM.recoveryGuiHeld())')
    control(7, 'complete')
    lua.execute('assert(not CM.autoSyncPump(100)); assert(speed==0 and changes==2 and didInitialUnpause)')
    lua.execute('CM.autoSyncPump(100); assert(changes==2)')
    control(8, 'holding')
    lua.execute("CM.detectInstance=function() return true end; CM.gameTime=function() return 100 end; CM.ticks=0; CM.pollEvents=function() error('producer reached') end")
    main=(Path(__file__).resolve().parents[1]/'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(encoding='utf-8')
    begin=main.index('update = function()')+len('update = function()')
    end=main.index('pcall(CM.sampleSimRate)',begin)
    lua.execute('function worker() '+main[begin:end]+" error('producer reached') end")
    lua.execute('worker()')
    available=directory/'tpf2_sync_available.txt'
    available.write_text('pid=999\nprotocol=4\nwall='+str(int(time.time()))+'\n')
    lua.execute('assert(not CM.syncRequest("sync_request"))')
    available.write_text('pid=123\nprotocol=4\nwall=1\n')
    lua.execute('assert(not CM.syncRequest("sync_request"))')
    available.write_text('pid=123\nprotocol=4\nwall='+str(int(time.time()))+'\n')
    lua.execute('assert(CM.syncRequest("sync_request"))')
    request=(directory/'tpf2_sync_request.txt').read_text()
    assert request.startswith('pid=123\ncmd=sync_request\nid=') and request.endswith('\n')
    # The Resync section of the Multiplayer window: real callbacks over widget stand-ins.
    lua.execute("""
        local function widget(text)
            return {text=text,visible=true,enabled=true,
                setText=function(self,t) self.text=t end,
                setEnabled=function(self,v) self.enabled=v end,
                setVisible=function(self,v) self.visible=v end,
                onClick=function(self,f) self.click=f end,
                addItem=function() end,setLayout=function() end}
        end
        api={gui={comp={TextView={new=widget},Button={new=widget},Component={new=widget}},
            layout={BoxLayout={new=widget}}}}
        function dash(t) t.boot=tostring(os.time()); t.wall=tostring(os.time()); t.resynctoken='world'; return t end
        assert(CM.resyncSection() == CM.resyncBox and not CM.resyncBox.visible)
        assert(not CM.resyncGuiTick(dash({desyncs='0'})) and not CM.resyncBox.visible)
        -- a desync without an operation: only Resync now
        assert(CM.resyncGuiTick(dash({desyncs='1'})) and CM.resyncBox.visible)
        assert(CM.resyncText.text:find('out of sync', 1, true))
        assert(CM.resyncButton.visible and CM.resyncButton.enabled)
        CM.resyncButton.click()
        assert(not CM.resyncButton.enabled and CM.resyncText.text:find('requested', 1, true))
        CM.resyncGuiTick(dash({desyncs='1'}))
        assert(not CM.resyncButton.enabled)
    """)
    clicked=(directory/'tpf2_sync_request.txt').read_text()
    assert clicked != request and 'cmd=sync_request\n' in clicked and 'operation=' not in clicked
    # While the world is held the section only reports (the native gate swallows
    # its clicks): no button, phase text from the control file, error step and detail.
    control(9, 'transferring')
    lua.execute("""
        assert(CM.resyncGuiTick(dash({desyncs='1',resync='1',resyncstatus='transferring'})))
        assert(CM.resyncText.text:find('Transferring the save', 1, true) and not CM.resyncButton.visible)
    """)
    control(10, 'error', step='checking', detail='Fresh worlds differ')
    lua.execute("""
        assert(CM.resyncGuiTick(dash({desyncs='1',resync='1',resyncstatus='error'})))
        assert(CM.resyncText.text:find('World comparison failed', 1, true) and CM.resyncText.text:find('Fresh worlds differ', 1, true))
        assert(CM.resyncText.text:find('Multiplayer Resync panel', 1, true) and not CM.resyncButton.visible)
    """)
    assert (directory/'tpf2_sync_request.txt').read_text() == clicked
    control(11, 'complete')
    lua.execute("""
        assert(not CM.resyncGuiTick(dash({desyncs='0'})) and not CM.resyncBox.visible)
        -- a stale dash keeps the last view instead of flipping it
        assert(not CM.resyncGuiTick({desyncs='1'}))
        -- a later desync in the same world offers Resync now again
        assert(CM.resyncGuiTick(dash({desyncs='1'})) and CM.resyncButton.visible and CM.resyncButton.enabled)
    """)
print('PASS: real Lua 5.2 producer hold, fresh paused comparison, stale/partial IPC, PID guard, pause preservation and the Resync section of the Multiplayer window; engine simulated')
