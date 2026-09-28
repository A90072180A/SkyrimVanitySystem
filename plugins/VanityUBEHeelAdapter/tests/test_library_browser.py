"""Read-only active-index browser tests; synthetic bytes, no user game assets."""
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock
from zipfile import ZipFile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from offline import bulk_library as b
from offline.library_browser import LibraryBrowser


class BrowserTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.lib=self.root/'height-library';self.lib.mkdir();self.data=self.root/'Data'
        self.shoes=[b.identity(f'{name}.esp|00000801',f'{name}.esp|00000800',f'!UBE\\{name}.nif') for name in ('Glass','Flat')]
        self.stocks=[b.identity(f'Socks.esp|{0x800+i:08X}','Socks.esp|00000800',f'!UBE\\socks{i}.nif') for i in range(3)]
        self.entries=[]
        for shoe in self.shoes:
            assets=[]
            for resource in [shoe[2],*(s[2] for s in self.stocks),'!ube\\socks.tri']:
                payload=resource.encode();p=self.data/'Meshes'/Path(resource.replace('\\','/'));p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(payload)
                assets.append(dict(resource=resource,fingerprint=b.fnv(payload)))
            members=[dict(stocking=s,group=(0 if i<2 else 1),flags=(3 if i==0 else 1 if i==1 else 0),originalNoHeel=0.,originalHeel=(1.09 if i==0 else 1.1 if i==1 else .8),residual=.2 if i<2 else .1,sources=[0,i+1,4]) for i,s in enumerate(self.stocks)]
            sh=dict(shoe=shoe,weight=0.,assets=assets,groups=[(0.,1.1),(0.,.8)],members=members)
            raw=b.encode_shard(sh);name=hashlib.sha256(raw).hexdigest()+'.vhs';(self.lib/name).write_bytes(raw)
            self.entries.append(dict(shoe=shoe,file=name,fingerprint=b.fnv(raw),count=len(members)))
        self.write_index()
    def tearDown(self):self.temp.cleanup()
    def write_index(self,generation='g1',entries=None):
        (self.lib/'index.vhi').write_bytes(b.encode_index(self.entries if entries is None else entries,generation,self.stocks))
    def test_index_only_and_one_shoe_reads(self):
        lib=LibraryBrowser(self.lib);self.assertEqual(lib.shard_reads,0)
        (self.lib/self.entries[1]['file']).unlink() # unselected broken shard does not prevent viewing first
        self.assertEqual(len(lib.shoes('glass !ube')),1)
        r=lib.read_shoe(self.shoes[0]);self.assertEqual(lib.shard_reads,1);lib.read_shoe(self.shoes[0]);self.assertEqual(lib.shard_reads,1)
        self.assertEqual(len(r['members']),3)
    def test_shared_values_and_originals_flags(self):
        r=LibraryBrowser(self.lib).read_shoe(self.shoes[0])
        self.assertEqual(r['groups'][0]['memberIndexes'],[0,1]);self.assertEqual(r['members'][0]['Heel'],1.1)
        self.assertEqual(r['members'][0]['originalHeel'],1.09);self.assertTrue(r['members'][0]['sharedApproximation'])
        self.assertIsNone(r['members'][0]['residualAtAppliedValue']);self.assertEqual(r['members'][1]['residualAtAppliedValue'],.2)
    def test_orphans_not_enrolled(self):
        self.write_index(entries=self.entries[:1]);(self.lib/'orphan.vhs').write_bytes(b'bad')
        lib=LibraryBrowser(self.lib);self.assertEqual(len(lib.shoes()),1)
        with self.assertRaises(ValueError):lib.read_shoe(self.shoes[1])
    def test_copies_are_independent(self):
        lib=LibraryBrowser(self.lib);r=lib.read_shoe(self.shoes[0]);r['members'][0]['Heel']=2
        self.assertEqual(lib.read_shoe(self.shoes[0])['members'][0]['Heel'],1.1)
    def test_stale_index_rejected(self):
        lib=LibraryBrowser(self.lib);lib.read_shoe(self.shoes[0]);self.write_index('new')
        with self.assertRaisesRegex(ValueError,'已更新'):lib.read_shoe(self.shoes[0])
    def test_integrity_and_count_checks(self):
        p=self.lib/self.entries[0]['file'];p.write_bytes(p.read_bytes()+b'x')
        with self.assertRaisesRegex(ValueError,'指纹'):LibraryBrowser(self.lib).read_shoe(self.shoes[0])
    def test_corrupt_index_rejected(self):
        (self.lib/'index.vhi').write_bytes(b'bad')
        with self.assertRaises(ValueError):LibraryBrowser(self.lib)
    def test_names_do_not_change_identity(self):
        lib=LibraryBrowser(self.lib);r=lib.shoes('高跟', {self.shoes[0]:'玻璃高跟'})
        self.assertEqual(len(r),1);self.assertEqual(r[0]['shoe'],self.shoes[0])
    def test_export_readonly_and_exact_precision(self):
        before={p.name:p.read_bytes() for p in self.lib.iterdir()}
        p=self.root/'shoe.json';LibraryBrowser(self.lib).export_report(self.shoes[0],p)
        r=json.loads(p.read_text());self.assertEqual(r['members'][0]['originalHeel'],1.09)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.lib.iterdir()})
    def test_forbid_existing_and_library_destinations(self):
        lib=LibraryBrowser(self.lib)
        for path in (self.lib/'out.json',self.lib/'index.vhi'):
            with self.assertRaises(ValueError):lib.export_report(self.shoes[0],path)
        p=self.root/'x.json';p.write_text('keep')
        with self.assertRaises(ValueError):lib.export_report(self.shoes[0],p)
        self.assertEqual(p.read_text(),'keep')
    def test_bundle_selected_sources_only(self):
        lib=LibraryBrowser(self.lib);p=self.root/'models.zip';manifest=lib.bundle_models(self.shoes[0],[0],self.data,p)
        with ZipFile(p) as z:
            self.assertEqual(len(z.namelist()),4);self.assertFalse(any('flat.nif' in n or 'socks1' in n for n in z.namelist()))
            self.assertEqual(json.loads(z.read('DIAGNOSTIC-MANIFEST.json'))['totalBytes'],manifest['totalBytes'])
    def test_bundle_change_rejected_no_output(self):
        p=self.data/'Meshes/!ube/glass.nif';p.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'不一致'):LibraryBrowser(self.lib).bundle_models(self.shoes[0],[0],self.data,self.root/'out.zip')
        self.assertFalse((self.root/'out.zip').exists());self.assertFalse(list(self.root.glob('*.tmp')))
    def test_bundle_selection_bounds(self):
        lib=LibraryBrowser(self.lib)
        for ids in ([],[0,0],[-1],[3],[True]):
            with self.assertRaises(ValueError):lib.bundle_models(self.shoes[0],ids,self.data,self.root/'out.zip')
    def test_all_members_large_linear_grouping(self):
        entry=self.entries[0];sh=b.decode_shard((self.lib/entry['file']).read_bytes());m=sh['members'][0]
        sh['members']=[];sh['groups']=[];stocks=[]
        for i in range(5000):
            n=copy.deepcopy(m);s=b.identity(f's.esp|{i+2048:08X}',m['stocking'][1],m['stocking'][2]);stocks.append(s)
            n.update(stocking=s,group=i);sh['members'].append(n);sh['groups'].append((0.,i/5000.))
        raw=b.encode_shard(sh);name=hashlib.sha256(raw).hexdigest()+'.vhs';(self.lib/name).write_bytes(raw)
        (self.lib/'index.vhi').write_bytes(b.encode_index([dict(shoe=sh['shoe'],file=name,fingerprint=b.fnv(raw),count=5000)],'big',stocks))
        r=LibraryBrowser(self.lib).read_shoe(sh['shoe']);self.assertEqual(len(r['groups']),5000)


class GuiTests(BrowserTests):
    # Only the tests defined below count as Tk tests, not inherited core tests.
    def setUp(self):
        super().setUp()
        import tkinter as tk
        try:root=tk.Tk();root.destroy()
        except tk.TclError:
            if os.environ.get('VHA_REQUIRE_TK'):raise
            self.skipTest('Tk display unavailable')
        import argparse, vha_library
        self.args=argparse.Namespace(library=self.lib,plugins=None,user_file=None,data=self.data,shoe='')
        self.win=vha_library.gui(self.args,run_loop=False);self.drain()
    def tearDown(self):
        if hasattr(self,'win'):
            self.win.vha['close']()
        super().tearDown()
    def drain(self):
        end=time.monotonic()+5
        while time.monotonic()<end:
            self.win.update()
            if not self.win.vha['state']['busy']:return
            time.sleep(.02)
        self.fail('GUI worker did not finish')
    def test_gui_select_shoe_group_member(self):
        v=self.win.vha;self.assertEqual(len(v['shoes'].get_children()),2)
        v['shoes'].selection_set('0');v['read_selected']();self.drain()
        self.assertEqual(len(v['members'].get_children()),3)
        v['groups'].selection_set('0');v['show_members']();self.assertEqual(len(v['members'].get_children()),2)
        v['all_members']();self.assertEqual(len(v['members'].get_children()),3)
        v['member_query'].set('socks2');self.assertEqual(len(v['members'].get_children()),1)
    def test_gui_stale_shoe_does_not_leave_old_rows(self):
        v=self.win.vha;v['shoes'].selection_set('0');v['read_selected']();self.drain();self.write_index('new')
        with mock.patch('tkinter.messagebox.showerror') as popup:
            v['read_selected']();self.drain();self.assertTrue(popup.called)
        self.assertIsNone(v['state']['report']);self.assertEqual(len(v['members'].get_children()),0)
    def test_gui_export_and_bundle_actions(self):
        v=self.win.vha;v['shoes'].selection_set('0');v['read_selected']();self.drain()
        with mock.patch('tkinter.filedialog.asksaveasfilename',return_value=str(self.root/'ui.json')):
            v['export']();self.drain()
        self.assertTrue((self.root/'ui.json').exists())
        v['members'].selection_set('0')
        with mock.patch('tkinter.messagebox.askyesno',return_value=True),mock.patch('tkinter.filedialog.asksaveasfilename',return_value=str(self.root/'ui.zip')):
            v['bundle']();self.drain()
        self.assertTrue((self.root/'ui.zip').exists())

# Avoid rerunning inherited non-GUI cases through GuiTests.
for _name in list(BrowserTests.__dict__):
    if _name.startswith('test_'):setattr(GuiTests,_name,None)

if __name__=='__main__':unittest.main()
