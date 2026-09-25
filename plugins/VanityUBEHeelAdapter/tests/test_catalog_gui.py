"""Use the shipped Tk actions, not a stand-in GUI. Requires an available display."""
import json
import os
from pathlib import Path
import time
from types import SimpleNamespace
import unittest
from unittest import mock
import test_offline as fixtures
from test_catalog_query import row
import vha_offline

class GuiTests(unittest.TestCase):
    write = fixtures.OfflineTests.write
    fixture = fixtures.OfflineTests.fixture

    def setUp(self):
        import tkinter as tk
        try:
            probe=tk.Tk();probe.destroy()
        except tk.TclError as e:
            if os.environ.get('VHA_REQUIRE_TK'):raise
            raise unittest.SkipTest('No Tk display: '+str(e))
        fixtures.OfflineTests.setUp(self)
        self.fixture();self.errors=[]
        self.patches=[mock.patch('tkinter.messagebox.showerror',side_effect=lambda *a,**k:self.errors.append(a)),
                      mock.patch('tkinter.messagebox.showinfo',return_value=None),
                      mock.patch('tkinter.messagebox.askyesno',return_value=True)]
        for p in self.patches:p.start()
        args=SimpleNamespace(data=self.data,profile=self.profile,output=self.root/'out',weight=0.,
            anchor='Flat.esp|00000801',preset=None,preset_name=None,archive_list=None,
            race=None,mods_root=None,heel_max=2.,plugins=self.root/'dest',user_file=None)
        self.window=vha_offline.gui(args,run_loop=False);self.app=self.window.vha
        self.window.update()

    def tearDown(self):
        self.app['close']()
        for p in self.patches:p.stop()
        fixtures.OfflineTests.tearDown(self)

    def wait(self):
        deadline=time.monotonic()+15
        while self.app['state']['busy'] and time.monotonic()<deadline:
            self.window.update();time.sleep(.01)
        self.window.update()
        self.assertFalse(self.app['state']['busy']);self.assertEqual(self.errors,[])

    def scan(self):self.app['scan']();self.wait()

    def test_scope_compute_apply_and_anchor_not_target(self):
        self.scan();stock,shoe,_=self.app['panels']
        self.assertEqual(str(self.app['compute_button']['state']),'disabled')
        stock.select_filtered();shoe.vars['name'].set('High');shoe.refresh();shoe.select_filtered()
        self.window.update()
        self.assertIn('1 条丝袜 × 1 双鞋',self.app['scope'].get())
        self.assertEqual(str(self.app['compute_button']['state']),'normal')
        self.app['recommend']();self.wait()
        r=self.app['state']['report'];self.assertEqual(len(r['entries']),1)
        self.assertEqual(r['entries'][0]['footwear'],'High.esp|00000801')
        self.app['candidates'].selection_set(['0']);self.app['apply']();self.wait()
        user=json.loads((self.root/'dest/VanityUBEHeelAdapter.user.json').read_text(encoding='utf-8'))
        self.assertEqual(len(user['pairs']),1)

    def test_hidden_selection_does_not_leak_to_plan(self):
        self.scan();stock,shoe,_=self.app['panels'];stock.select_filtered();shoe.select_filtered()
        self.window.update();self.assertEqual(len(shoe.selected_keys()),2)
        shoe.vars['name'].set('High');shoe.refresh();self.window.update()
        self.assertEqual(len(shoe.selected_keys()),1)
        shoe.vars['name'].set('');shoe.refresh();self.window.update()
        self.assertEqual(len(shoe.selected_keys()),1)
        shoe.clear_selection();self.window.update()
        self.assertEqual(str(self.app['compute_button']['state']),'disabled')

    def test_budget_rejected_in_ui_before_worker(self):
        rows=[row('S'+str(i)) for i in range(228)]+[row('F'+str(i),'footwear') for i in range(220)]
        self.app['state']['scanner']=SimpleNamespace(rows=rows)
        self.app['populate'](rows)
        for panel in self.app['panels'][:2]:panel.select_filtered()
        self.window.update()
        self.assertIn('50,160',self.app['scope'].get())
        self.assertEqual(str(self.app['compute_button']['state']),'disabled')
        self.assertFalse(self.app['state']['busy'])

    def test_sort_and_pages_keep_exact_ids(self):
        rows=[row('S'+str(i)) for i in range(650)]
        self.app['populate'](rows);panel=self.app['panels'][0]
        panel.select_filtered();self.window.update()
        self.assertEqual(len(panel.selected_keys()),650)
        self.assertEqual(len(panel.tree.get_children()),300)
        panel.turn(1);self.window.update();panel.sort('name');self.window.update()
        self.assertEqual(len(panel.selected_keys()),650)
        panel.tree.selection_set([]);panel.changed();self.window.update()
        self.assertEqual(len(panel.selected_keys()),350)

    def test_save_restore_filters(self):
        self.scan();panel=self.app['panels'][0];panel.vars['model'].set('!UBE')
        panel.vars['name'].set('heel | 高跟 | 丝袜 | cosplay')
        path=self.root/'filters.json'
        with mock.patch('tkinter.filedialog.asksaveasfilename',return_value=str(path)):self.app['save_filters']()
        panel.clear_text();self.window.update()
        with mock.patch('tkinter.filedialog.askopenfilename',return_value=str(path)):self.app['load_filters']()
        self.window.update();self.assertEqual(panel.vars['model'].get(),'!UBE')
        self.assertEqual(panel.vars['name'].get(),'heel | 高跟 | 丝袜 | cosplay')

    def test_catalog_preview_cannot_compute(self):
        self.scan()
        with mock.patch('tkinter.filedialog.askopenfilename',return_value=str(self.root/'out/offline-catalog.json')):self.app['browse']()
        self.wait()
        self.assertIsNone(self.app['state']['scanner'])
        for panel in self.app['panels'][:2]:panel.select_filtered()
        self.window.update();self.assertEqual(str(self.app['compute_button']['state']),'disabled')
        self.assertIn('预览',self.app['message'].get())

if __name__=='__main__':unittest.main(verbosity=2)
