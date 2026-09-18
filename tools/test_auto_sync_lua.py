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
    # 3 is a real engine speed (a session sat at it, 2026-09-16); 7 is not
    path.write_text(path.read_text().replace('resume_speed=0', 'resume_speed=7'))
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
    # WORLD TOKEN: the first sim tick of every world stamps which world this
    # game is in, addressed to this game's own pid. The host's menu reads a
    # CHANGE as "it loaded another world" and pushes that world to everyone
    # (a NEW GAME reaches no other hook), so one load must write exactly one
    # value and two loads must never write the same one. It is written before
    # the recovery hold above, so a resync's own load is stamped while the menu
    # still knows it is busy and does not mistake it for the player's doing.
    token=directory/'tpf2mp_world_gen.txt'
    first=token.read_text()
    assert first.startswith('pid=123\ngen=') and first.endswith('\n') and len(first.split('\n')[1])>8, first
    lua.execute('worker()')                       # same world, same tick loop: one value per load
    assert token.read_text()==first
    reboot=LuaRuntime(unpack_returned_tuples=True)   # another world = another Lua state
    reboot.globals().base=str(directory).replace('\\','/')+'/'
    reboot.execute('CM={ticks=0}; K={BASE=base,PROCESS_ID="123",INSTANCE="a"}\n'
                   'CM.detectInstance=function() return true end\n'
                   'CM.gameTime=function() return 100 end\n'
                   'CM.autoSyncPump=function() return true end')
    reboot.execute('function worker() '+main[begin:end]+' end')
    reboot.execute('worker()')
    second=token.read_text()
    assert second.startswith('pid=123\ngen=') and second!=first, (first, second)
    available=directory/'tpf2_sync_available.txt'
    available.write_text('pid=999\nprotocol=4\nwall='+str(int(time.time()))+'\n')
    lua.execute('assert(not CM.syncRequest("sync_request"))')
    available.write_text('pid=123\nprotocol=4\nwall=1\n')
    lua.execute('assert(not CM.syncRequest("sync_request"))')
    available.write_text('pid=123\nprotocol=4\nwall='+str(int(time.time()))+'\n')
    lua.execute('assert(CM.syncRequest("sync_request"))')
    request=(directory/'tpf2_sync_request.txt').read_text()
    assert request.startswith('pid=123\ncmd=sync_request\nid=') and request.endswith('\n')
    # GUI publishes notices without constructing a second window or requesting recovery.
    lua.execute("""
        function dash(t) t.boot=tostring(os.time()); t.wall=tostring(os.time()); t.resynctoken='world'; return t end
        CM.resyncGuiTick(dash({desyncs='0'}))
    """)
    notice=directory/'tpf2_sync_notice.txt'
    assert 'desyncs=0\n' in notice.read_text()
    lua.execute("CM.resyncGuiTick(dash({desyncs='1'}))")
    assert 'desyncs=1\nheld=0\n' in notice.read_text()
    assert 'pid=123\nworld=world\n' in notice.read_text()
    assert (directory/'tpf2_sync_request.txt').read_text() == request
    lua.execute("CM.resyncGuiTick(dash({resync='1',resyncstatus='loading'}))")
    assert 'held=1\n' in notice.read_text()
    previous=notice.read_text()
    lua.execute("CM.resyncGuiTick({desyncs='1'})")
    assert notice.read_text() == previous
    lua.execute("CM.resyncGuiTick(dash({desyncs='0'}))")
    assert 'desyncs=0\nheld=0\n' in notice.read_text()
    # ALONE (2026-09-17): a hold whose other players are gone (roster 1, no peer heard) is abandoned
    # after K.SOLO_RELEASE_TICKS and the lever comes back; with others present it is held for good.
    lua.execute("alone=false; CM.syncLobbyAlive=function() return not alone end; CM.rosterPlayers=nil; K.SOLO_RELEASE_TICKS=75; "
                "CM.ticks=1000; speed=3; changes=0; CM.autoSync=nil; CM.autoReleased=nil; CM.resyncHold=false")
    control(20, 'loading')
    lua.execute("assert(CM.autoSyncPump(100) and speed==0 and CM.resyncHold and CM.autoResumeSpeed==3)")
    lua.execute("for i=1,200 do CM.ticks=CM.ticks+1; assert(CM.autoSyncPump(100)) end; assert(speed==0 and CM.resyncHold)")
    lua.execute("alone=true; for i=1,75 do CM.ticks=CM.ticks+1; assert(CM.autoSyncPump(100)) end; assert(speed==0)")  # the clock starts at the first alone tick
    lua.execute("CM.ticks=CM.ticks+1; assert(not CM.autoSyncPump(100)); assert(speed==3 and not CM.resyncHold and CM.autoSync==nil)")
    lua.execute("assert(not CM.autoSyncPump(100)); assert(speed==3)")   # stays released: the stale file is not re-read as new
    lua.execute("alone=false; CM.autoAloneSince=nil")
    (directory / 'tpf2_sync_lua.txt').write_text('pid=123\noperation=' + 'c' * 32 + '\nepoch=' + 'b' * 32
                                                 + '\nrevision=21\nphase=loading\nresume_speed=0\n')
    lua.execute("assert(CM.autoSyncPump(100) and speed==0)")            # a fresh operation with others present holds again
    lua.execute("alone=true; for i=1,40 do CM.ticks=CM.ticks+1 end; alone=false; assert(CM.autoSyncPump(100)); "
                "alone=true; for i=1,74 do CM.ticks=CM.ticks+1; assert(CM.autoSyncPump(100)) end; assert(speed==0)")  # the alone clock restarts
    # THE FROZEN JOIN OF 21:51 (2026-09-17): a freshly loaded Lua state hears no peer and knows no roster yet,
    # but the lobby is alive -- that is not alone, however long it lasts
    lua.execute("alone=false; CM.autoAloneSince=nil; CM.rosterPlayers=nil; CM.peers={}; "
                "for i=1,600 do CM.ticks=CM.ticks+1; assert(CM.autoSyncPump(100)) end; assert(speed==0 and CM.resyncHold)")
    # a roster the lobby says is one player, lobby still alive: that IS alone (the other player left)
    lua.execute("CM.rosterPlayers=1; for i=1,75 do CM.ticks=CM.ticks+1; assert(CM.autoSyncPump(100)) end; "
                "CM.ticks=CM.ticks+1; assert(not CM.autoSyncPump(100)); assert(speed==3)")
    # the real liveness reader: a fresh wall is alive, a stale one is not, a missing file is not
    lua.execute("CM.syncLobbyAlive=nil")
    lua.execute(source)(lua.globals().CM, lua.globals().K, lambda _: None)   # reload the module's functions
    (directory / 'tpf2_sync_available.txt').write_text('protocol=4\nwall=%d\npid=123\n' % int(time.time()))
    lua.execute("CM.syncLobbyAt=nil; assert(CM.syncLobbyAlive()); CM.rosterPlayers=nil; assert(not CM.syncAlone())")
    (directory / 'tpf2_sync_available.txt').write_text('protocol=4\nwall=%d\npid=123\n' % (int(time.time()) - 60))
    lua.execute("CM.syncLobbyAt=nil; assert(not CM.syncLobbyAlive()); assert(CM.syncAlone())")
    (directory / 'tpf2_sync_available.txt').unlink()
    lua.execute("CM.syncLobbyAt=nil; assert(not CM.syncLobbyAlive()); CM.rosterPlayers=3; assert(CM.syncAlone())")
print('PASS: real Lua 5.2 producer hold, fresh paused comparison, stale/partial IPC, PID guard, pause preservation, '
      'one fresh world token per load and native-panel notices, a hold abandoned once alone; engine simulated')
