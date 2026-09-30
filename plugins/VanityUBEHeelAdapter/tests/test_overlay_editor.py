import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from overlay_ui import OverlaySession, decode, gui
import height_editor

SOCK='[Caenarvon] Cosplay Basics.esp|00000D1A'
SHOE='Glass High-Heeled Shoes.esp|00000804'

class CoreTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.path=self.root/'VanityUBEHeelAdapter.user.json'
        self.doc={'schema':1,'settings':{'heelMax':2.,'enableStockingFootOcclusion':True,'enableGlassFootFit':True},
            'items':[{'armor':SHOE,'kind':'footwear','coverage':'opaque-closed','note':'旧标记'}],
            'pairs':[{'stocking':SOCK,'footwear':SHOE,'mode':'manual','NoHeel':0.,'Heel':1.1,'note':'已确认'},
                     {'stocking':SOCK,'footwear':'<barefoot>','mode':'manual','NoHeel':1.,'Heel':0.}]}
        self.path.write_text(json.dumps(self.doc),encoding='utf-8')
    def tearDown(self):self.temp.cleanup()
    def test_edit_existing_preserves_other_content_and_backup(self):
        before=self.path.read_bytes();s=OverlaySession(self.root)
        doc=s.put_pair(s.document,SOCK,SHOE,0.,1.15,note='微调',index=0)
        s.save(json.dumps(doc));self.assertEqual(s.document['pairs'][1],self.doc['pairs'][1])
        self.assertEqual(s.document['items'],self.doc['items']);self.assertEqual(self.path.with_suffix('.json.bak').read_bytes(),before)
        self.assertEqual(s.document['pairs'][0]['Heel'],1.15)
    def test_preserve_approval_and_reject_identity_swap(self):
        approval={k:'bound' for k in height_editor.APPROVAL};approval.update(stockingAddon=SOCK,footwearAddon=SHOE)
        self.doc['pairs'][0]['approval']=approval;self.path.write_text(json.dumps(self.doc))
        s=OverlaySession(self.root);j=s.put_pair(s.document,SOCK,SHOE,0,1.12,index=0)
        self.assertEqual(j['pairs'][0]['approval'],approval)
        with self.assertRaises(ValueError):s.put_pair(s.document,SOCK,'Other.esp|00000001',0,1.1,index=0)
    def test_cannot_replace_newer_other_editor(self):
        s=OverlaySession(self.root);new=self.path.read_bytes()+b'\n';self.path.write_bytes(new)
        with self.assertRaises(RuntimeError):s.save(s.text())
        self.assertEqual(self.path.read_bytes(),new)
    def test_invalid_raw_never_changes_file(self):
        before=self.path.read_bytes();s=OverlaySession(self.root)
        bad=['{','{"schema":1,"schema":1}', '{"pairs":[],"settings":{"heelMax":NaN}}']
        for update in ({'settings':{'typo':True}},{'pairs':[dict(self.doc['pairs'][0],Heel=8)]},
                       {'pairs':[dict(self.doc['pairs'][0],NoHeel=.5)]}, {'pairs':self.doc['pairs']*2},
                       {'settings':{'heelMax':True}},{'items':[dict(self.doc['items'][0],kind='stocking')]}):
            bad.append(json.dumps({**self.doc,**update}))
        for raw in bad:
            with self.assertRaises((ValueError,TypeError)):s.save(raw)
            self.assertEqual(self.path.read_bytes(),before)
        self.assertFalse(self.path.with_suffix('.json.bak').exists())
    def test_native_limits_checked_before_save(self):
        s=OverlaySession(self.root);before=self.path.read_bytes()
        for update in ({'schema':True},{'pairs':[dict(self.doc['pairs'][0],note='汉'*700)]},
                       {'pairs':[dict(self.doc['pairs'][0],stocking='a'*300+'.esp|00000001')]},
                       {'items':[dict(self.doc['items'][0],note='x'*(1024*1024))]}):
            with self.assertRaises(ValueError):s.save(json.dumps({**self.doc,**update}))
            self.assertEqual(before,self.path.read_bytes())
    def test_raw_settings_maximum_and_disable_fit(self):
        s=OverlaySession(self.root);doc=copy.deepcopy(self.doc);doc['settings'].update(heelMax=3.,enableGlassFootFit=False)
        doc['pairs'][0]['Heel']=2.5;s.save(json.dumps(doc));self.assertEqual(s.document['pairs'][0]['Heel'],2.5)
        self.assertFalse(s.document['settings']['enableGlassFootFit'])
    def test_ignore_and_delete_restore_automatic(self):
        s=OverlaySession(self.root);j=s.put_pair(s.document,SOCK,SHOE,0,0,mode='ignore',index=0)
        self.assertNotIn('Heel',j['pairs'][0]);s.save(json.dumps(j));del j['pairs'][0];s.save(json.dumps(j))
        self.assertEqual(len(s.document['pairs']),1)
    def test_wrong_file_and_wrong_live_destination_refused(self):
        with self.assertRaises(ValueError):OverlaySession(self.root,self.root/'offline-candidates.json')
        out=self.root/'VanityUBEHeelAdapter';out.mkdir()
        (out/'configuration-status.json').write_text(json.dumps({'schema':1,'heartbeatUnixMs':time.time()*1000,
            'userPath':str(self.root/'active'/'VanityUBEHeelAdapter.user.json')}))
        s=OverlaySession(self.root,self.path)
        with self.assertRaises(RuntimeError):s.save(s.text())
    def test_create_overlay_without_scanning_or_game_logs(self):
        self.path.unlink();s=OverlaySession(self.root)
        j=s.put_pair(s.document,SOCK,SHOE,0,1.1);s.save(json.dumps(j));self.assertTrue(self.path.exists())
        self.assertFalse((self.root/'VanityUBEHeelAdapter').exists())
    def test_duplicate_keys_on_disk_rejected_without_rewrite(self):
        raw=b'{"schema":1,"pairs":[],"pairs":[]}'
        self.path.write_bytes(raw)
        with self.assertRaises(ValueError):OverlaySession(self.root)
        self.assertEqual(self.path.read_bytes(),raw)
    def test_editor_reads_one_consistent_snapshot(self):
        with mock.patch('height_editor.read_json', wraps=height_editor.read_json) as read:
            s=OverlaySession(self.root)
        self.assertFalse(any(call.args[0]==self.path for call in read.call_args_list))
        self.assertEqual(s.document,self.doc)
    def test_atomic_replace_failure_leaves_original(self):
        s=OverlaySession(self.root);before=self.path.read_bytes()
        with mock.patch('height_editor.os.replace',side_effect=PermissionError('locked')):
            with self.assertRaises(OSError):s.save(s.text())
        self.assertEqual(before,self.path.read_bytes());self.assertFalse(list(self.root.glob('*.tmp')))

class GuiTests(CoreTests):
    def setUp(self):
        super().setUp()
        import tkinter as tk
        try:probe=tk.Tk();probe.destroy()
        except tk.TclError as exc:
            if os.environ.get('VHA_REQUIRE_TK'):raise
            raise unittest.SkipTest(str(exc))
        self.errors=[]
        self.patches=[mock.patch('tkinter.messagebox.showerror',side_effect=lambda *a,**k:self.errors.append(a)),
            mock.patch('tkinter.messagebox.askyesno',return_value=True)]
        for p in self.patches:p.start()
        self.window=gui(self.root,run_loop=False);self.app=self.window.vha;self.window.update()
    def tearDown(self):
        if hasattr(self,'app'):
            self.app['close']();self.app.clear();self.app=None;self.window=None
            import gc;gc.collect()
            for p in self.patches:p.stop()
        super().tearDown()
    def test_actual_existing_pair_widgets_and_save(self):
        self.app['tree'].selection_set('0');self.window.update()
        self.assertEqual(self.app['fields']['Heel'].get(),'1.1')
        self.app['fields']['Heel'].set('1.14');self.app['put']();self.app['save']();self.window.update()
        self.assertFalse(self.errors);self.assertEqual(json.loads(self.path.read_text())['pairs'][0]['Heel'],1.14)
    def test_gui_raw_edit_and_conflict(self):
        text=self.app['raw'];text.delete('1.0','end');text.insert('1.0',json.dumps(self.doc))
        self.path.write_text(json.dumps({**self.doc,'settings':{'heelMax':3}}))
        self.app['save']();self.assertEqual(len(self.errors),1)
        self.assertEqual(json.loads(self.path.read_text())['settings']['heelMax'],3)
    def test_stale_list_cannot_edit_changed_json(self):
        self.app['tree'].selection_set('0');self.window.update()
        self.app['raw'].insert('end',' ');self.app['put']();self.assertEqual(len(self.errors),1)
        self.assertEqual(json.loads(self.path.read_text())['pairs'][0]['Heel'],1.1)

class ScannerEntryTests(unittest.TestCase):
    def test_path_restart_profile_switch_and_direct_editor_without_scan(self):
        import tkinter as tk
        try:probe=tk.Tk();probe.destroy()
        except tk.TclError as exc:
            if os.environ.get('VHA_REQUIRE_TK'):raise
            raise unittest.SkipTest(str(exc))
        import vha_offline
        from offline.engine import DEFAULT_ANCHOR
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder);prefs=base/'memory.json';dest=base/'plugins';dest.mkdir()
            args=SimpleNamespace(data=None,profile=None,mods_root=None,plugins=None,user_file=None,output=None,
                weight=0.,anchor=DEFAULT_ANCHOR,settings_file=prefs,heel_max=2.)
            root=vha_offline.gui(args,run_loop=False);app=root.vha
            app['paths']['data'].set(str(base/'Data'));app['paths']['profile'].set('profile-A')
            app['paths']['plugins'].set(str(dest));app['paths']['output'].set(str(base/'out'))
            self.assertTrue(app['remember_paths']());app['close']();app.clear();root=None
            for key in ('plugins','user_file','output'):setattr(args,key,None)
            root=vha_offline.gui(args,run_loop=False);app=root.vha;root.update()
            self.assertEqual(app['paths']['plugins'].get(),str(dest));self.assertIsNone(app['state']['scanner'])
            child=app['edit_manual_overlay']();self.assertIsNotNone(child)
            self.assertEqual(child.vha['state']['session'].path,dest/'VanityUBEHeelAdapter.user.json')
            child.vha['raw'].insert('end',' ')
            with mock.patch('tkinter.messagebox.askyesno',return_value=False):
                app['close']();self.assertTrue(root.winfo_exists());self.assertTrue(child.winfo_exists())
            with mock.patch('tkinter.messagebox.askyesno',return_value=True):child.vha['close']()
            child.vha.clear();child=None
            app['paths']['profile'].set('profile-B');root.update();self.assertEqual(app['paths']['plugins'].get(),'')
            app['paths']['profile'].set('profile-A');root.update();self.assertEqual(app['paths']['plugins'].get(),str(dest))
            app['paths']['data'].set(str(base/'OtherData'));root.update();self.assertEqual(app['paths']['plugins'].get(),'')
            app['close']();app.clear();root=None
            import gc;gc.collect()

if __name__=='__main__':unittest.main()
