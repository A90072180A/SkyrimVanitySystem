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
        # Tk variables must be finalized on the owning thread, not during a
        # later test's geometry-worker allocation/garbage collection.
        self.app.clear();self.app=None;self.window=None
        import gc
        gc.collect()
        for p in self.patches:p.stop()
        fixtures.OfflineTests.tearDown(self)

    def wait(self):
        deadline=time.monotonic()+15
        while self.app['state']['busy'] and time.monotonic()<deadline:
            self.window.update();time.sleep(.01)
        self.window.update()
        self.assertFalse(self.app['state']['busy']);self.assertEqual(self.errors,[])

    def scan(self):self.app['scan']();self.wait()

    def test_primary_bulk_export_keeps_user_overlay_empty(self):
        from offline.bulk_library import decode_index
        self.scan();stock,shoe,_=self.app['panels'];stock.select_filtered();shoe.select_filtered()
        self.app['recommend']();self.wait()
        self.assertTrue(self.app['allow'].get())
        self.app['select_applicable']();self.app['export_selected']();self.wait()
        index=self.root/'dest/VanityUBEHeelAdapter/height-library/index.vhi'
        doc=decode_index(index.read_bytes())
        self.assertEqual(sum(e['count'] for e in doc['entries']),2)
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())

    def test_load_saved_candidates_can_export_without_rescan(self):
        self.scan();stock,shoe,_=self.app['panels'];stock.select_filtered();shoe.select_filtered()
        self.app['recommend']();self.wait();path=self.app['state']['reportPath']
        self.app['state'].update(scanner=None,report=None,reportSaved=False,reportPath=None)
        with mock.patch('tkinter.filedialog.askopenfilename',return_value=str(path)):
            self.app['load_candidates']();self.wait()
        self.assertIsNone(self.app['state']['scanner'])
        self.app['select_applicable']();self.app['export_selected']();self.wait()
        self.assertTrue((self.root/'dest/VanityUBEHeelAdapter/height-library/index.vhi').exists())

    def test_shared_group_button_exports_actual_members(self):
        from offline.bulk_library import decode_index, decode_shard
        self.scan();stock,shoe,_=self.app['panels'];stock.select_filtered();shoe.select_filtered()
        self.app['recommend']();self.wait()
        group=self.app['group_preview']();group.update();g=group.vha
        g['tree'].selection_set([str(r['groupIndex']) for r in g['state']['report']['groups']])
        g['export_groups']();self.wait();group.destroy()
        folder=self.root/'dest/VanityUBEHeelAdapter/height-library'
        index=decode_index((folder/'index.vhi').read_bytes())
        self.assertEqual(sum(len(decode_shard((folder/e['file']).read_bytes())['members']) for e in index['entries']),2)

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

    def selected_compute(self):
        self.scan();stock,shoe,_=self.app['panels']
        stock.select_filtered();shoe.select_filtered();self.window.update()
        self.app['recommend']();self.wait()

    def test_saved_compact_report_group_preview_does_not_change_values(self):
        self.selected_compute()
        r=self.app['state']['report'];path=self.root/'out/offline-candidates.json'
        before=path.read_bytes()
        self.assertEqual(json.loads(before)['schema'],2)
        group_window=self.app['group_preview']();self.window.update()
        g=group_window.vha['state']['report']
        self.assertFalse(g['runtimeReadable']);self.assertFalse(g['automaticApplicationAllowed'])
        group_window.vha['tolerance'].set('0.02');group_window.vha['cross'].set(True)
        group_window.vha['refresh']();self.window.update()
        self.assertEqual(path.read_bytes(),before)
        self.assertTrue((self.root/'out/offline-shoe-groups.json').is_file())
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())
        group_window.destroy()

    def test_failed_publication_retains_results_and_retries_without_recompute(self):
        self.scan();stock,shoe,_=self.app['panels']
        stock.select_filtered();shoe.select_filtered();self.window.update()
        with mock.patch('offline.engine.write_candidates',side_effect=PermissionError('synthetic locked file')):
            self.app['recommend']()
            deadline=time.monotonic()+10
            while self.app['state']['busy'] and time.monotonic()<deadline:
                self.window.update();time.sleep(.01)
            self.window.update()
        self.assertEqual(len(self.errors),1);self.errors.clear()
        self.assertIsNotNone(self.app['state']['report']);self.assertFalse(self.app['state']['reportSaved'])
        self.app['candidates'].selection_set(['0']);self.app['apply']();self.window.update()
        self.assertFalse(self.app['state']['busy'])
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())
        with mock.patch('offline.engine.geo.fit',side_effect=AssertionError('must not recompute')):
            self.app['retry_save']();self.wait()
        self.assertTrue(self.app['state']['reportSaved'])
        self.assertEqual(json.loads((self.root/'out/offline-candidates.json').read_bytes())['schema'],2)

    def test_grouping_ignores_rejected_rows_and_selected_scope_is_explicit(self):
        self.selected_compute()
        self.app['candidates'].selection_set(['0'])
        window=self.app['group_preview']();self.window.update()
        report=window.vha['state']['report']
        self.assertEqual([i for g in report['groups'] for i in g['memberIndexes']],[0])
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())
        window.destroy()

if __name__=='__main__':unittest.main(verbosity=2)
