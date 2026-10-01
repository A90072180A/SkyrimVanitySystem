"""Exercise the shipped Tk entry point, including nonmeasurable manual shoes."""
import copy
import gc
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
import vha_offline
from offline.engine import DEFAULT_ANCHOR
from offline.foot_reference import TOOL_VERSION

class FeetGuiTests(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        try:probe=tk.Tk();probe.destroy()
        except tk.TclError as exc:
            if os.environ.get('VHA_REQUIRE_TK'):raise
            raise unittest.SkipTest(str(exc))
        self.temp=tempfile.TemporaryDirectory();self.rootdir=Path(self.temp.name)
        self.plugins=self.rootdir/'plugins';self.plugins.mkdir()
        self.user=self.plugins/'VanityUBEHeelAdapter.user.json'
        self.doc={'pairs':[{'stocking':'Example.esp|00000801','footwear':'Glass.esp|00000804',
                           'mode':'manual','NoHeel':0.,'Heel':1.1}]}
        self.user.write_text(json.dumps(self.doc),encoding='utf-8')
        self.args=SimpleNamespace(data=self.rootdir/'Data',profile=self.rootdir/'profile',mods_root=None,
            plugins=self.plugins,user_file=None,output=self.rootdir/'out',weight=0.,anchor=DEFAULT_ANCHOR,
            settings_file=self.rootdir/'memory.json',heel_max=2.,preset=None,preset_name=None,archive_list=None,race=None)
        self.errors=[]
        self.patch=mock.patch('tkinter.messagebox.showerror',side_effect=lambda *a,**k:self.errors.append(a))
        self.patch.start();self.confirm=mock.patch('tkinter.messagebox.askyesno',return_value=True);self.confirm.start()
        self.window=vha_offline.gui(self.args,run_loop=False);self.app=self.window.vha
        self.stock={'key':'s','kind':'stocking','status':'measurable','UBE':True,'name':'CPB',
          'armor':'[Caenarvon] Cosplay Basics.esp|00000D1A','addon':'[Caenarvon] Cosplay Basics UBE patch.esp|0000093B',
          'model':'!UBE\\Caenarvon\\Cosplay\\cpb_sb_1.nif'}
        self.shoe={'key':'w','kind':'footwear','status':'no-reference-feet','UBE':True,'name':'Witchy',
          'armor':'Witchy Agata Heels.esp|00000800','addon':'Witchy Agata Heels UBE patch.esp|00000801',
          'model':'!UBE\\[Spaz490]\\Witchy Agata Heels\\witchy_1.nif'}
        self.app['populate']([self.stock,self.shoe]);self.window.update()
        sock,shoe,_=self.app['panels'];shoe.vars['status'].set('全部状态');shoe.refresh();self.window.update()
        for panel,key in ((sock,'s'),(shoe,'w')):
            panel.tree.focus(key);panel.tree.selection_set(key)
        self.window.update()

    def tearDown(self):
        self.app['close']();self.app.clear();self.window.vha.clear();self.app=None;self.window=None
        gc.collect();self.patch.stop();self.confirm.stop();self.temp.cleanup()

    def test_unmeasurable_shoe_can_seed_manual_but_not_compute(self):
        self.assertEqual(self.app['panels'][1].selected_keys(),[])
        child=self.app['edit_manual_overlay']()
        self.assertIsNotNone(child);self.assertEqual(child.vha['fields']['footwear'].get(),self.shoe['armor'])
        self.assertEqual(json.loads(self.user.read_text()),self.doc)
        self.assertEqual(str(self.app['compute_button']['state']),'disabled')
        child.vha['close']();child.vha.clear()

    def test_witchy_trial_not_saved_until_explicit_action_and_no_clobber(self):
        child=self.app['edit_manual_overlay'](trial=True);self.assertIsNotNone(child)
        a=child.vha
        self.assertEqual(a['fields']['Heel'].get(),'0.47');self.assertIn('UNVALIDATED',a['fields']['note'].get())
        self.assertEqual(json.loads(self.user.read_text()),self.doc)
        a['put']();self.assertEqual(json.loads(self.user.read_text()),self.doc)
        a['save']();self.assertFalse(self.errors)
        result=json.loads(self.user.read_text());self.assertEqual(result['pairs'][0],self.doc['pairs'][0])
        self.assertEqual(result['pairs'][1]['Heel'],.47);self.assertNotIn('approval',result['pairs'][1])
        a['new_pair']();a['put']();self.assertEqual(len(self.errors),1)
        self.assertEqual(len(json.loads(self.user.read_text())['pairs']),2)
        a['close']();a.clear()

    def test_trial_wrong_identity_refused(self):
        self.app['panels'][1].by_key['w']['armor']='Other.esp|00000800'
        self.assertIsNone(self.app['edit_manual_overlay'](trial=True));self.assertEqual(len(self.errors),1)
        self.assertEqual(json.loads(self.user.read_text()),self.doc)

    def test_tool_version_and_path_persistence_unchanged(self):
        self.assertIn(TOOL_VERSION,self.window.title())
        self.assertTrue(self.app['remember_paths']())
        self.assertTrue(self.args.settings_file.is_file())

if __name__=='__main__':unittest.main(verbosity=2)
