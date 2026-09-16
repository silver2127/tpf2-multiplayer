"""Real UDP relay upload -> mod cache -> initial join and hotjoin, no game processes."""
import json, os, socket, sys, tempfile, threading, time
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'netpunch'))
import lobby as l
import modshare as m

def main():
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp);src=root/'source';src.mkdir();(src/'mod.lua').write_text('function data() return {} end')
        save=root/'world.sav';save.write_bytes(b'world'*4096);Path(str(save)+'.lua').write_text('["lockstep.lua"] = {}')
        mods=[['*9876543210',1],['_urbangames_deluxe_pack',1],['_urbangames_preorder_pack',1]]
        stop=threading.Event();threads=[];conns=[];ios={}
        hs=l.open_socket(0,socket.AF_INET);port=hs.getsockname()[1]
        def installed(mid,v):
            if m.is_dlc(mid) or threading.current_thread().name=='leader': return str(src)
            p=root/threading.current_thread().name/'mods'/m.mod_folder_name(mid,v).replace("*","workshop_")
            return str(p) if (p/'mod.lua').exists() else None
        def catalogue():
            return threading.current_thread().name,{(mid,str(v)) for mid,v in mods if installed(mid,v)}
        def command(io,msg):
            with open(io.in_path,'a',encoding='utf8') as f: f.write(json.dumps(msg)+'\n')
        def connect(name):
            io=l.LobbyIO(str(root/name));ios[name]=io
            s=l.open_socket(0,socket.AF_INET)
            peer={'candidates':{'public_v4':f'127.0.0.1:{port}','lan_v4':None,'v6':None},'flags':{'open':True,'v6':False}}
            c=l.race(s,peer,'dial',s.getsockname()[1],8,my_has_v6=False);assert c,'connect failed'
            conns.append(c)
            t=threading.Thread(target=l.run_client,name=name,args=(c,name,io),kwargs={'stop':stop},daemon=True);t.start();threads.append(t)
        with patch.object(m,'save_mod_list',return_value=mods), patch.object(m,'find_mod',return_value=str(src)), patch.object(m,'installed_mod',side_effect=installed), patch.object(m,'install_target',side_effect=lambda mid,v:str(root/threading.current_thread().name/'mods'/m.mod_folder_name(mid,v).replace("*","workshop_"))), patch.object(m,'request_catalogue',side_effect=lambda:threading.current_thread().name), patch.object(m,'catalogue',side_effect=catalogue):
            server=l.LobbyIO(str(root/'relay'))
            t=threading.Thread(target=l.run_host,args=(hs,'relay',server),kwargs={'relay_only':True,'stop':stop},daemon=True);t.start();threads.append(t)
            try:
                connect('leader');connect('joiner')
                assert l._wait_until(lambda:len((l._latest_roster(ios['leader'].out_path) or {}).get('players',[]))==2,10),'roster'
                command(ios['leader'],{'cmd':'start','save':str(save)})
                answered={}
                def ready(name):
                    events=l._read_events(ios[name].out_path)
                    count=sum(e.get('type')=='mods_prompt' for e in events)
                    if count>answered.get(name,0): answered[name]=count;command(ios[name],{'cmd':'mods','accept':True})
                    return l._has_start(ios[name].out_path,save=True)
                assert l._wait_until(lambda:ready('joiner'),25),'initial relay mod download'
                cache=root/'relay/mod_cache'
                assert len(list(cache.glob('*.zip')))==1,'DLC must not enter cache'
                connect('late')
                assert l._wait_until(lambda:ready('late'),25),'relay hotjoin mod download'
                for name in ('joiner','late'):
                    events=l._read_events(ios[name].out_path)
                    registered=next(i for i,e in enumerate(events) if e.get('type')=='mods_ready')
                    started=next(i for i,e in enumerate(events) if e.get('type')=='start')
                    assert registered<started
                print('PASS: relay caches one mod, excludes DLC, initial join and hotjoin register before start')
            finally:
                stop.set()
                for c in conns:c.close()
                for t in threads:t.join(3)
if __name__=='__main__':main()
