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

def push(r,sid,kind,files,mods=None):
    blob=b''.join(data for _,data in files)
    metadata=[dict(name=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()) for name,data in files]
    chunk=lobby.CHUNK_LOCAL
    r.on_begin(dict(sid=sid,kind=kind,files=metadata,total_bytes=len(blob),total_chunks=(len(blob)+chunk-1)//chunk,chunk=chunk,sha256=hashlib.sha256(blob).hexdigest(),mods=mods or []))
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
