"""Deterministic reliable-queue tests using the production sender and receiver.
No Steam/game/network needed. FIFO has finite byte capacity, bandwidth and delay;
control ACKs overtake it. --source PATH --measure compares an older checkout.
"""
import argparse, collections, hashlib, json, pathlib, struct, sys
from unittest.mock import patch
ap=argparse.ArgumentParser();ap.add_argument('--source',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[1]);ap.add_argument('--measure',action='store_true');args=ap.parse_args()
sys.path.insert(0,str(args.source/'netpunch'))
import lobby
lobby.STEAM_BIG_CHUNKS = True  # exercise the optional reliable path, independent of local flags

class IO:
    def emit(self,e): pass

def scenario(rate, drops=(), ack_gap=False):
    now=[100.0]; messages=[]; wire=collections.deque(); feedback=collections.deque()
    blob=bytes(range(256))* (12*1024*1024//256) + b'partial last chunk'
    peer=('127.0.0.1',62100); capacity=8*1024*1024
    stats={'offered':0,'refused':0,'peak_queue':0}; queued=0; free=now[0]; lost=set(drops)
    class Socket:
        def sendto(self,frame,addr):
            nonlocal queued,free
            assert frame[5:9]==lobby.CHUNK_MAGIC
            sid,seq=struct.unpack('!II',frame[9:17]);data=frame[17:]
            stats['offered']+=len(data)
            if seq in lost:
                lost.remove(seq);return len(frame) # loss on local UDP leg
            if queued+len(frame)>capacity:
                stats['refused']+=1;return len(frame) # Steam rejects but UDP send succeeded
            free=max(free,now[0])+len(frame)/rate
            wire.append((free+0.05,sid,seq,data,len(frame)));queued+=len(frame)
            stats['peak_queue']=max(stats['peak_queue'],queued)
            return len(frame)
    with patch.object(lobby.time,'time',side_effect=lambda:now[0]):
        snd=lobby._HostSaveTransfer(Socket(),123,blob,[],[(peer,'fixture')],IO(),messages.append)
        rcv=lobby._ClientSaveReceiver(None,IO(),messages.append)
        rcv.sid=123;rcv.chunk=snd.chunk;rcv.window=snd.window;rcv.total_chunks=snd.total_chunks
        rcv.total_bytes=len(blob);rcv.buf=bytearray(len(blob));rcv.have=bytearray(snd.total_chunks)
        rcv._finalize=lambda:setattr(rcv,'complete',True)
        def feedback_send(msg):
            if msg.get('t')=='fack' and not (ack_gap and 103<now[0]<108):feedback.append((now[0]+0.05,msg))
        rcv._send=feedback_send
        snd.on_begin_ack(peer,{'sid':123})
        for tick in range(40000):
            now[0]=100+tick/100
            while wire and wire[0][0]<=now[0]:
                _,sid,seq,data,n=wire.popleft();queued-=n;rcv.on_chunk(sid,seq,data)
            while feedback and feedback[0][0]<=now[0]:snd.on_fack(peer,feedback.popleft()[1])
            if tick%5==0:rcv._send_fack()
            snd.pump(now[0])
            if rcv.complete or snd.peers[peer]['state']=='failed':break
        same=rcv.complete and hashlib.sha256(rcv.buf).digest()==hashlib.sha256(blob).digest()
        result=dict(rate_Bps=rate,drop_count=len(drops),ack_gap=ack_gap,seconds=round(now[0]-100,2),complete=same,duplicates=rcv.duplicate_chunks if hasattr(rcv,'duplicate_chunks') else None,**stats)
        result['overhead_ratio']=round(stats['offered']/len(blob),3)
        print(json.dumps(result))
        if not args.measure:
            assert same,result
            assert result['overhead_ratio'] < 1.10 + len(drops)*snd.chunk/len(blob),result
            assert stats['peak_queue']<=capacity,result
            assert not stats['refused'],result
            assert result['seconds'] < len(blob)/rate+40,result
    return result

for rate,drops,gap in [(256*1024,(),False),(1024*1024,(),False),(16*1024*1024,(),False),(1024*1024,(0,64,128),False),(1024*1024,(),True),(1024*1024,tuple(range(0,394,7)),False)]:scenario(rate,drops,gap)
if not args.measure:print('PASS: bounded reliable queue, missing local datagrams, lost feedback, exact hash')

def recovery_guards():
    """No burst on early NACK, delayed readiness, rewind, dead-peer timeout."""
    clock = [100.0]
    frames = []
    class Socket:
        def sendto(self, frame, addr):
            frames.append(frame)
            return len(frame)
    peer = ('127.0.0.1', 62100)
    with patch.object(lobby.time, 'time', side_effect=lambda: clock[0]):
        sender = lobby._HostSaveTransfer(Socket(), 7, b'x' * (32000 * 256), [],
                                         [(peer, 'fixture')], IO(), lambda _: None)
        clock[0] = 125.0
        sender.on_begin_ack(peer, {'sid': 7})
        sender.pump(clock[0])
        initial = len(frames)
        clock[0] = 125.05
        sender.on_fack(peer, {'sid': 7, 'base': 0, 'nack': list(range(128))})
        sender.pump(clock[0])
        assert len(frames) == initial, 'early NACK duplicated the first window'
        clock[0] = 129.0
        sender.pump(clock[0])
        assert len(frames) == initial + 1, 'a stalled FIFO needs only one probe'
        sender.on_fack(peer, {'sid': 7, 'base': 16, 'nack': list(range(16, 144))})
        sender.pump(clock[0])
        sender.on_fack(peer, {'sid': 7, 'base': 0, 'nack': list(range(128))})
        before = len(frames)
        sender.pump(clock[0])
        assert len(frames) - before == lobby.STEAM_WINDOW_START, 'hash rewind did not restart'
        clock[0] = 160.0
        sender.on_fack(peer, {'sid': 7, 'base': 0, 'nack': list(range(128))})
        sender.pump(clock[0])
        assert sender.peers[peer]['state'] == 'failed', 'feedback alone kept a dead transfer alive'
    print('PASS: early feedback, delayed readiness, hash rewind, stalled-peer timeout')

if not args.measure:
    recovery_guards()
