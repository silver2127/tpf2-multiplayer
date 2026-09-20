"""Consent, catalogue barrier, DLC exclusion and relay-cache regression tests."""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'netpunch'))
import lobby
import modshare

class Conn:
    def __init__(self): self.sent=[]
    def send(self,raw): self.sent.append(json.loads(raw))
class IO:
    def __init__(self,root): self.dir=str(root); self.events=[]
    def emit(self,event): self.events.append(event)

def archive():
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w') as z: z.writestr('mod.lua','function data() return {} end')
    return out.getvalue()

def begin(r,sid,kind,files,mods=None,batch=None):
    """The host's fbegin for ``files`` (a mods batch when ``batch`` is [k, n]); returns the blob."""
    blob=b''.join(data for _,data in files)
    metadata=[dict(name=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()) for name,data in files]
    chunk=lobby.CHUNK_LOCAL
    msg=dict(sid=sid,kind=kind,files=metadata,total_bytes=len(blob),total_chunks=(len(blob)+chunk-1)//chunk,chunk=chunk,sha256=hashlib.sha256(blob).hexdigest(),mods=mods or [])
    if batch: msg['batch']=batch
    r.on_begin(msg)
    return blob

def push(r,sid,kind,files,mods=None,batch=None):
    blob=begin(r,sid,kind,files,mods,batch)
    chunk=lobby.CHUNK_LOCAL
    for seq in range((len(blob)+chunk-1)//chunk): r.on_chunk(sid,seq,blob[seq*chunk:(seq+1)*chunk])
    r.settle()   # the verify/write runs on a worker thread; apply its outcome before asserting

class Downloads(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        self.patch=patch.object(modshare,'data_dir',return_value=str(self.root));self.patch.start()
        self.conn=Conn();self.io=IO(self.root);self.r=lobby._ClientSaveReceiver(self.conn,self.io,lambda _:None)
    def tearDown(self): self.patch.stop();self.tmp.cleanup()
    def offer(self): self.r.on_manifest([['*9876543210',1]])
    def accept_and_install(self):
        self.offer();self.r.answer_mods(True)
        push(self.r,10,'mods',[(modshare.mod_zip_name('*9876543210',1),archive())])
    def test_cancel_leaves_and_cannot_install(self):
        self.offer();self.r.answer_mods(False)
        push(self.r,10,'mods',[(modshare.mod_zip_name('*9876543210',1),archive())])
        self.assertTrue(self.r.cancelled);self.assertIn({'t':'leave'},self.conn.sent)
        self.assertFalse((self.root/'workshop').exists())
    def test_download_waits_for_matching_catalogue_receipt(self):
        self.accept_and_install();self.assertFalse(self.r.complete)
        self.assertTrue((self.root/'workshop/9876543210/mod.lua').exists())
        self.assertFalse(any(m.get('t')=='fdone' and m.get('ok') for m in self.conn.sent))
        (self.root/'mods_catalogue.txt').write_text('stale\n*9876543210\t1\n')
        self.r.tick(time.time());self.assertFalse(self.r.complete)
        (self.root/'mods_catalogue.txt').write_text(self.r.catalogue_token+'\n*9876543210\t1\n')
        self.r.tick(time.time());self.assertTrue(self.r.complete and self.r.mods_satisfied)
    def test_mid_round_batch_keeps_announcing_done(self):
        """A batch that is not the last of its round leaves the round open (complete
        stays false) -- and until 2026-09-20 its done went out exactly once, so one
        lost datagram had the host time the peer out and fail the round (batch
        18/359 of a 556-mod round, unpacked in 0 s). The done is re-announced on
        the tick and in answer to a resent chunk, until the next batch begins."""
        self.offer();self.r.answer_mods(True)
        files=[(modshare.mod_zip_name('*9876543210',1),archive())]
        push(self.r,10,'mods',files,batch=[1,2])
        self.assertFalse(self.r.complete);self.assertTrue(self.r.batch_open);self.assertTrue(self.r.batch_done)
        dones=lambda sid:[m for m in self.conn.sent if m.get('t')=='fdone' and m.get('ok') and m.get('sid')==sid]
        self.assertEqual(len(dones(10)),1)
        now=time.time()
        self.r.tick(now+1);self.r.tick(now+2)
        self.assertEqual(len(dones(10)),3,'re-announced on the tick')
        self.r.on_chunk(10,0,b'x')
        self.assertEqual(len(dones(10)),4,'a resent chunk is answered with done')
        self.assertTrue((self.root/'workshop/9876543210/mod.lua').exists())
        # the next batch's fbegin ends the announcement: no done for either sid while it is incoming
        self.conn.sent.clear()
        begin(self.r,11,'mods',files,batch=[2,2])
        self.assertFalse(self.r.batch_done)
        self.r.tick(now+3);self.r.on_chunk(10,0,b'x')
        self.assertFalse(dones(10));self.assertFalse(dones(11))
    def on_disk(self):
        """A Workshop folder for *9876543210 that is on this PC but not in the game's catalogue."""
        folder=self.root/'steam_ws'/'9876543210';folder.mkdir(parents=True);(folder/'mod.lua').write_text('x')
        return patch.object(modshare,'on_disk_mod',side_effect=lambda mid,v:str(folder) if mid=='*9876543210' else None)
    def receipt(self,*names):
        (self.root/'mods_catalogue.txt').write_text(self.r.catalogue_token+'\n'+''.join(n.rsplit('_',1)[0]+'\t'+n.rsplit('_',1)[1]+'\n' for n in names))
    def test_on_disk_mod_is_registered_not_downloaded(self):
        """A required mod whose folder is here but uncatalogued is named in the
        registry and the game is asked to refresh; nothing is offered or requested.
        Until 2026-09-20 it was zipped, sent and found 'present' on arrival."""
        with self.on_disk():
            self.offer()
            self.assertFalse(self.r.ask);self.assertEqual(self.r.need,[]);self.assertFalse(self.r.mods_satisfied)
            self.assertTrue(self.r.catalogue_token);self.assertFalse(self.r.round_receipt)
            self.assertEqual([m for m,v,_ in self.r.registering],['*9876543210'])
            self.assertFalse(any(e['type']=='mods_prompt' for e in self.io.events))
            self.assertTrue(any(e['type']=='mods_refresh' for e in self.io.events))
            token,rows=modshare.read_registry()
            self.assertEqual(token,self.r.catalogue_token);self.assertEqual(rows,{'9876543210':str(self.root/'steam_ws'/'9876543210')})
            self.r.tick(time.time());self.assertFalse(self.r.mods_satisfied,'no receipt yet')
            self.receipt('*9876543210_1');self.r.tick(time.time())
            self.assertTrue(self.r.mods_satisfied);self.assertIsNone(self.r.catalogue_token);self.assertFalse(self.r.complete)
            self.assertTrue(any(e['type']=='mods_ready' for e in self.io.events))
            self.assertFalse(any(m.get('t') in ('mods_request','fdone') for m in self.conn.sent))
    def test_catalogued_workshop_mods_still_get_a_registry_row(self):
        """A Workshop mod the game lists AND that is on disk gets its folder into the
        registry without a refresh: the game's own entry for the id may lack a
        folder, and the plugin puts ours in its place at the next refresh."""
        folder=self.root/'steam_ws'/'9876543210';folder.mkdir(parents=True);(folder/'mod.lua').write_text('x')
        with patch.object(modshare,'installed_mod',return_value=str(folder)), patch.object(modshare,'on_disk_mod',return_value=str(folder)):
            (self.root/'mods_registry.txt').write_text('b'*32+'\n')
            self.offer()
            self.assertFalse(self.r.ask);self.assertEqual(self.r.need,[]);self.assertIsNone(self.r.catalogue_token)
            self.assertTrue(self.r.mods_satisfied)
            token,rows=modshare.read_registry()
            self.assertEqual(token,'b'*32,'no new token: nothing to wait for');self.assertEqual(rows,{'9876543210':str(folder)})
    def test_registration_the_game_does_not_recognise_disconnects(self):
        with self.on_disk():
            self.offer();self.receipt();self.r.tick(time.time())
        self.assertTrue(self.r.cancelled)
    def test_on_disk_and_absent_mods_register_then_download_the_rest(self):
        """Only what is nowhere on this PC is offered; the request for it waits for
        the registration receipt, then goes out."""
        with self.on_disk():
            self.r.on_manifest([['*9876543210',1],['*1111111111',1]])
            self.assertEqual(self.r.need,['*1111111111_1']);self.assertTrue(self.r.ask)
            self.r.answer_mods(True)
            self.conn.sent.clear();self.r.tick(time.time()+2)
            self.assertFalse(any(m['t']=='mods_request' for m in self.conn.sent),'the request waits for the receipt')
            self.receipt('*9876543210_1');self.r.tick(time.time()+3)
            self.assertFalse(self.r.mods_satisfied);self.assertIsNone(self.r.catalogue_token)
            self.r.tick(time.time()+5)
            self.assertEqual([m['need'] for m in self.conn.sent if m['t']=='mods_request'],[['*1111111111_1']])
    def test_mid_round_batch_publishes_the_registry(self):
        """Each batch's installs reach the registry at once (token kept), so a
        round that never finishes still registers them at the next game start."""
        self.offer();self.r.answer_mods(True)
        (self.root/'mods_registry.txt').write_text('a'*32+'\n')
        push(self.r,10,'mods',[(modshare.mod_zip_name('*9876543210',1),archive())],batch=[1,2])
        token,rows=modshare.read_registry()
        self.assertEqual(token,'a'*32,'a mid-round publish keeps the token');self.assertEqual(list(rows),['9876543210'])
        self.assertIsNone(self.r.catalogue_token,'no receipt is asked for mid-round')
    def test_registration_beside_a_save_transfer_keeps_feeding_it(self):
        """A save whose fbegin names an on-disk uncatalogued mod registers it; the
        save keeps streaming meanwhile (facks flow) and the start waits for the receipt."""
        with self.on_disk():
            data=b'save'*4096
            blob=begin(self.r,20,'save',[('incoming_save.sav',data)],mods=[['*9876543210',1]])
            self.assertEqual(self.r.need,[]);self.assertTrue(self.r.catalogue_token);self.assertFalse(self.r.round_receipt)
            self.conn.sent.clear();self.r.tick(time.time()+1)
            self.assertTrue(any(m['t']=='fack' for m in self.conn.sent),'the transfer is fed during a registration')
            chunk=lobby.CHUNK_LOCAL
            for seq in range((len(blob)+chunk-1)//chunk): self.r.on_chunk(20,seq,blob[seq*chunk:(seq+1)*chunk])
            self.r.settle();self.assertTrue(self.r.complete);self.assertTrue(self.r.save_done)
            self.assertTrue(self.r.catalogue_token,'the receipt is still awaited')
            self.receipt('*9876543210_1');self.r.tick(time.time()+2)
            self.assertIsNone(self.r.catalogue_token);self.assertTrue(self.r.mods_satisfied and self.r.complete)
    def test_a_batch_of_several_mods_unpacks_them_all(self):
        """Every zip of a batch lands (they unpack on MODS_UNPACK_THREADS threads)."""
        ids=['*9876543210','*9876543211','*9876543212','*9876543213','*9876543214']
        self.r.on_manifest([[i,1] for i in ids]);self.r.answer_mods(True)
        push(self.r,10,'mods',[(modshare.mod_zip_name(i,1),archive()) for i in ids],batch=[1,1])
        for i in ids: self.assertTrue((self.root/'workshop'/i[1:]/'mod.lua').exists(),i)
        self.assertEqual(sorted(self.r.install_result['installed']),sorted(i+'_1' for i in ids))
        self.assertTrue(self.r.catalogue_token)
    def test_receipt_missing_required_mod_disconnects(self):
        self.accept_and_install();(self.root/'mods_catalogue.txt').write_text(self.r.catalogue_token+'\n')
        self.r.tick(time.time());self.assertTrue(self.r.cancelled)
    def test_catalogue_timeout_disconnects(self):
        self.accept_and_install();self.r.tick(time.time()+60);self.assertTrue(self.r.cancelled)
    def test_preflight_retry_and_manifest_dedupe(self):
        self.offer();self.offer();self.assertEqual(1,len([e for e in self.io.events if e['type']=='mods_prompt']))
        self.r.answer_mods(True);self.r.tick(time.time()+2)
        self.assertEqual(2,len([m for m in self.conn.sent if m['t']=='mods_request']))
    def test_old_dialog_cannot_approve_changed_mod_list(self):
        self.offer(); old=self.r.consent_id
        self.r.on_manifest([['*9876543211',1]])
        self.r.answer_mods(True,old)
        self.assertTrue(self.r.ask)
        self.assertFalse(self.r.approved)
    def test_dlc_is_never_packaged_or_installed(self):
        for name in ('_urbangames_deluxe_pack','_urbangames_preorder_pack','urbangames_deluxe_pack','urbangames_preorder_pack'):
            with patch.object(modshare,'find_mod',side_effect=AssertionError('DLC must not be opened')):
                self.assertIsNone(modshare.package_mod(name,1))
            self.assertEqual(('failed',None),modshare.install_mod_zip(archive(),name,1))
    def test_missing_dlc_leaves_without_download_prompt(self):
        with patch.object(modshare,'installed_mod',return_value=None):
            self.r.on_manifest([['_urbangames_deluxe_pack',1]])
        self.assertTrue(self.r.cancelled)
        self.assertFalse(any(e['type']=='mods_prompt' for e in self.io.events))
    def test_dlc_present_does_not_need_download(self):
        with patch.object(modshare,'installed_mod',return_value='built-in'):
            self.r.on_manifest([['_urbangames_deluxe_pack',1],['_urbangames_preorder_pack',1]])
        self.assertFalse(self.r.cancelled or self.r.ask)
    def test_path_traversal_and_empty_existing_install(self):
        self.assertEqual(('failed',None),modshare.install_mod_zip(archive(),'..',1))
        target=self.root/'workshop/9876543210';target.mkdir(parents=True)
        self.assertEqual(('failed',None),modshare.install_mod_zip(archive(),'*9876543210',1))
    def test_relay_caches_mods_before_distributing_world(self):
        r=lobby._ClientSaveReceiver(self.conn,self.io,lambda _:None,server_cache=str(self.root/'cache'))
        with patch.object(lobby,'_save_has_mp_mod',return_value=True):
            push(r,1,'save',[(lobby.INCOMING_BASENAME+'.sav',b'world')],mods=[['*9876543210',1],['_urbangames_deluxe_pack',1]])
        self.assertTrue(r.save_done);self.assertFalse(r.mods_satisfied)
        self.assertEqual(['*9876543210_1'],r.need);self.assertFalse(r.ask)
        push(r,2,'mods',[(modshare.mod_zip_name('*9876543210',1),archive())])
        self.assertTrue(r.complete and r.mods_satisfied)
        self.assertTrue((self.root/'cache'/modshare.cache_name('*9876543210',1)).exists())

    def test_registry_has_no_row_cap(self):
        # 128 rows used to reject the WHOLE registry on the reader (workshop_register.cpp)
        # and raise here on the writer: every consented mod then went unregistered on
        # that peer and its game loaded a different mod set from everyone else's.
        managed=Path(modshare.managed_workshop()); managed.mkdir(parents=True,exist_ok=True)
        ids=[str(3000000000+i) for i in range(300)]
        for mid in ids:
            (managed/mid).mkdir(); (managed/mid/'mod.lua').write_text('function data() return {} end')
        (managed/'notamod').mkdir()   # not an id and no mod.lua: never a row
        token=modshare.request_catalogue()
        rows=(self.root/'mods_registry.txt').read_text(encoding='utf-8').split('\n')
        self.assertEqual(rows[0],token); self.assertEqual(rows[-1],'')
        self.assertEqual(sorted(r.split('\t')[0] for r in rows[1:-1]),sorted(ids))
        for r in rows[1:-1]:
            mid,folder=r.split('\t'); self.assertTrue(os.path.isfile(os.path.join(folder,'mod.lua')),folder)

if __name__=='__main__': unittest.main()
