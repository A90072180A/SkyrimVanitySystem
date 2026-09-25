"""Filtering, bounded explicit scope and opened-handle provenance regression tests."""
import os
from pathlib import Path
import unittest
from unittest import mock
import test_offline as fixtures
from offline.catalog_query import *
from offline.provenance import provider_name, annotate, opened_path
from offline.engine import Scanner, apply_report
from offline.formats import FormatError, bounded_read


def row(key='S',kind='stocking',**kwargs):
    return dict(key=key,armor='Original.esp|00000800',addon='Patch.esp|00000801',
                kind=kind,status='measurable',name='Cosplay 高跟 丝袜',editorID='aCPB_S',
                model=r'!UBE\Caenarvon\Cosplay\CPB_S_1.nif',**kwargs)

class QueryTests(unittest.TestCase):
    def test_literal_casefold_unicode(self):
        self.assertTrue(text_match('CosPlay 高跟丝袜','COSPLAY 丝袜'))
        self.assertFalse(text_match('CosPlay 高跟丝袜','COSPLAY 不存在'))
    def test_or_groups(self):
        self.assertTrue(text_match('Cosplay Socks','heel | 高跟 | 丝袜 | cosplay'))
        self.assertTrue(text_match('高跟鞋','boots，高跟;丝袜'))
        self.assertFalse(text_match('Cape','heel | 高跟 | 丝袜 | cosplay'))
    def test_path_slashes_and_literal_exclamation(self):
        self.assertTrue(text_match(r'!UBE\Caenarvon\Cosplay','!ube/caenarvon'))
        self.assertFalse(text_match('UBE\\Caenarvon','!UBE'))
    def test_no_regex_or_shell_operators(self):
        self.assertTrue(text_match('[Dint999] boots','[Dint999]'))
        self.assertFalse(text_match('Dint999 boots','[Dint999]'))
        self.assertFalse(text_match('stocking','.*'))
    def test_quote_phrase(self):
        self.assertTrue(text_match('Cosplay Basics foo','"Cosplay Basics" foo'))
        self.assertFalse(text_match('Basics Cosplay foo','"Cosplay Basics"'))
    def test_fields_are_and(self):
        r=row()
        self.assertTrue(CatalogFilter(name='cosplay | unknown',model='!UBE',plugin='patch').matches(r))
        self.assertFalse(CatalogFilter(name='cosplay',model='3ba').matches(r))
    def test_status_is_exact_or_named_group(self):
        r=row();r['status']='NoHeel-unavailable'
        self.assertFalse(CatalogFilter().matches(r));self.assertTrue(CatalogFilter(status='not-measurable').matches(r))
        r['status']='source-error: missing foo.nif'
        self.assertTrue(CatalogFilter(status='source-error').matches(r))
    def test_pose_kind_requires_real_classification(self):
        r=row(kind='footwear',footPose={'kind':'raised-foot-pose'})
        self.assertTrue(CatalogFilter(kind='raised-foot-pose').matches(r))
        r.pop('footPose');self.assertFalse(CatalogFilter(kind='raised-foot-pose').matches(r))
    def test_unknown_provenance_not_guessed_from_plugin(self):
        r=row();self.assertFalse(CatalogFilter(provider='Original').matches(r))
        self.assertTrue(CatalogFilter(plugin='Original').matches(r))
    def test_winning_plugin_filter(self):
        r=row(armorWinnerPlugin='LastPatch.esp')
        self.assertTrue(CatalogFilter(plugin='lastpatch').matches(r))
    def test_filter_removes_hidden_selection(self):
        a=row();b=row('B');b['name']='other';m=CatalogSelection([a,b]);m.select_filtered()
        m.filter(CatalogFilter(name='cosplay'));self.assertEqual(m.selected,{'S'})
        m.filter(CatalogFilter());self.assertEqual(m.selected,{'S'})
    def test_page_selection_preserves_other_page(self):
        m=CatalogSelection([row('A'),row('B')]);m.update_page(['A'],['A']);m.update_page(['B'],['B'])
        self.assertEqual(m.selected,{'A','B'});m.update_page(['A'],[]);self.assertEqual(m.selected,{'B'})
    def test_quarantine_cannot_be_selected(self):
        m=CatalogSelection([row('bad',blockedBy=['bad']),row('good')]);m.select_filtered()
        self.assertEqual(m.selected,{'good'})
    def test_empty_selection_is_not_all(self):
        rows=[row(),row('F','footwear')]
        self.assertFalse(pair_plan(rows,[],['F'])['allowed'])
        self.assertFalse(pair_plan(rows,['S'],[])['allowed'])
        self.assertEqual(pair_plan(rows,None,None)['count'],1)
    def test_bad_key_does_not_broaden_scope(self):
        with self.assertRaisesRegex(ValueError,'missing/ineligible'):pair_plan([row()],['typo'],[])
    def test_actual_report_scale_budget(self):
        rows=[row('S'+str(i)) for i in range(228)]+[row('F'+str(i),'footwear') for i in range(220)]
        plan=pair_plan(rows,None,None);self.assertEqual(plan['count'],50160);self.assertFalse(plan['allowed'])
        self.assertTrue(pair_plan(rows,['S0'],['F0','F1'])['allowed'])
    def test_provider_roots_and_prefix_boundaries(self):
        self.assertEqual(provider_name(r'\\?\D:\MO2\mods\Output\meshes\a.nif',r'D:\MO2\mods'),'Output')
        self.assertIsNone(provider_name(r'D:\MO2\mods-extra\Other\a.nif',r'D:\MO2\mods'))
        self.assertEqual(provider_name(r'D:\MO2\overwrite\meshes\a.nif',None,r'D:\MO2\overwrite'),'Overwrite')
    def test_provider_case_and_unc(self):
        self.assertEqual(provider_name(r'\\?\UNC\server\MO\mods\My Mod\a.nif',r'\\server\mo\mods'),'My Mod')
    def test_annotation_does_not_use_tri_provider(self):
        r=row(sources=[{'resource':'foo.nif','physicalPath':r'D:\MO2\mods\NIF Output\meshes\a.nif'},
                       {'resource':'foo.tri','physicalPath':r'D:\MO2\mods\TRI Mod\meshes\a.tri'}])
        annotate(r,r'D:\MO2\mods');self.assertEqual(r['modelProviderMods'],['NIF Output'])
    def test_unknown_virtual_path_kept_unknown(self):
        r=row(sources=[{'resource':'foo.nif','physicalPath':r'D:\Game\Data\meshes\a.nif'}])
        annotate(r,r'D:\MO2\mods');self.assertEqual(r['modelProviderState'],'unknown')

class IntegrationTests(unittest.TestCase):
    setUp = fixtures.OfflineTests.setUp
    tearDown = fixtures.OfflineTests.tearDown
    write = fixtures.OfflineTests.write
    fixture = fixtures.OfflineTests.fixture

def test_explicit_targets_and_anchor_outside_scope(self):
    self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan()
    sock=next(r for r in s.rows if r['kind']=='stocking')['key']
    shoe=next(r for r in s.rows if r['armor']=='High.esp|00000801')['key']
    report=s.recommend('Flat.esp|00000801',[sock],shoe_keys=[shoe])
    self.assertEqual(len(report['entries']),1);self.assertEqual(report['selection']['shoeKeys'],[shoe])
    self.assertEqual(report['entries'][0]['footwear'],'High.esp|00000801')
    apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])

def test_empty_refused_before_geometry_read(self):
    self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan()
    with mock.patch.object(s,'get_shape',side_effect=AssertionError('must not read')),self.assertRaises(FormatError):
        s.recommend('Flat.esp|00000801',[],shoe_keys=None)

def test_same_handle_provenance_normal_file(self):
    p=self.write('probe.bin',b'bytes')
    with p.open('rb') as f:
        got=opened_path(f)
        if os.name=='nt':self.assertIsNotNone(got);self.assertTrue(got.lower().endswith('probe.bin'))
    payload,physical=bounded_read(p,with_path=True);self.assertEqual(payload,b'bytes')
    if os.name=='nt':self.assertEqual(physical,got)
for fn in (test_explicit_targets_and_anchor_outside_scope,test_empty_refused_before_geometry_read,test_same_handle_provenance_normal_file):
    setattr(IntegrationTests,fn.__name__,fn)

if __name__=='__main__':unittest.main(verbosity=2)
