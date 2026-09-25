"""Lossless storage, 19,320-pair stress and publication failure regressions.

Only synthetic metadata/results are used here. This is not a replay of user
geometry or a claim that a suggested height was visually validated.
"""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import test_offline as fixtures
from offline.candidate_store import (pack, unpack, read_candidates, write_candidates,
                                    stream_json, canonical, LIMIT)
from offline.engine import Scanner, apply_report, VERSION, CandidateSaveError
from offline.shoe_groups import propose


def sample(nstock=3,nshoe=2,source_padding=0):
    # Model providers and TRIs repeat across pair records like an actual catalog.
    sources=[{'kind':'loose','path':'D:/Data/'+str(i)+'.nif','resource':'meshes/'+str(i)+'.nif',
              'sha256':hashlib.sha256(str(i).encode()).hexdigest(),'physicalPath':'D:/MO2/mods/'+'a'*source_padding}
             for i in range(nstock+nshoe+1)]
    rows=[]
    for a in range(nstock):
        for b in range(nshoe):
            rows.append({'index':len(rows),'stocking':f'Stock.esp|{a+2048:08X}',
                'stockingAddon':f'Stock.esp|{a+10000:08X}','stockingName':f'丝袜 {a}','stockingModel':f'!UBE/sock{a}.nif',
                'footwear':f'Shoe.esp|{b+2048:08X}','footwearAddon':f'Shoe.esp|{b+10000:08X}',
                'footwearName':f'Glass {b}','footwearModel':f'!UBE/shoe{b}.nif','reference':'Ref.esp|00000800::Ref.esp|00000900',
                'NoHeel':0.,'Heel':1.1+b*.000012345678,'rawHeel':1.1+b*.000012345678,
                'rawNoHeel':-.731245621,'normalizedResidual':.195159,'rmsResidual':.03,
                'coverage':.99999,'saturated':False,'status':'review-residual','requiresVisualReview':True,
                'topology':{'method':'exact-topology','removedVertices':0},
                'sources':[sources[a],sources[nstock+b],sources[-1]],
                'stockingResponse':{'family':'family-a','shape':'stocking','Heel':{'rmsDelta':3.,'maxDelta':6.},
                                    'NoHeel':{'rmsDelta':4.,'maxDelta':8.}}})
    return {'schema':1,'generatorVersion':VERSION,'context':{'weight':0.,'bodyMorphs':{}},
            'heelMax':2.,'NoHeelMaximum':1.,'inputSources':sources,'reference':{'armor':'Ref.esp|00000800'},
            'counts':{'review-residual':len(rows)},'entries':rows}


class StoreTests(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def test_exact_roundtrip_and_floats(self):
        r=sample();v=unpack(pack(r))
        self.assertEqual(r,v)
        for a,b in zip(r['entries'],v['entries']):
            self.assertEqual(a['Heel'].hex(),b['Heel'].hex())
        self.assertEqual(len(pack(r)['tables']['results']),2)
        self.assertEqual(len(pack(r)['tables']['responses']),1)
    def test_no_shared_mutable_results_after_decode(self):
        r=unpack(pack(sample()));r['entries'][0]['topology']['method']='edited'
        r['entries'][0]['sources'][0]['sha256']='bad';r['entries'][0]['stockingResponse']['family']='changed'
        self.assertEqual(r['entries'][2]['topology']['method'],'exact-topology')
        self.assertNotEqual(r['entries'][1]['sources'][0]['sha256'],'bad')
        self.assertEqual(r['entries'][2]['stockingResponse']['family'],'family-a')
    def test_missing_sources_and_null_controls_preserved(self):
        r=sample();row=r['entries'][0];row.pop('sources');row.pop('stockingResponse');row.update(NoHeel=None,Heel=None,status='unsupported-topology')
        r['counts']={'unsupported-topology':1,'review-residual':5}
        self.assertEqual(r,unpack(pack(r)))
    def test_manual_edit_preserves_raw_and_original_suggestion(self):
        r=sample();a=r['entries'][0];a['originalSuggestion']={'NoHeel':a['NoHeel'],'Heel':a['Heel'],'status':a['status']}
        a.update(Heel=.9,status='manual-adjusted',residualAtEditedValue=None)
        p=self.root/'c.json';write_candidates(p,r);back=read_candidates(p)
        self.assertEqual(back['entries'],r['entries']);self.assertEqual(back['counts']['manual-adjusted'],1)
        self.assertEqual(back['entries'][2]['Heel'],1.1)
    def test_invalid_references_negative_bool_and_index_order(self):
        for value in (-1,True,100000,'0'):
            with self.subTest(value=value):
                r=pack(sample());r['entries'][0][0]=value
                with self.assertRaises(ValueError):unpack(r)
        r=sample();r['entries'][0]['index']=5
        with self.assertRaises(ValueError):pack(r)
    def test_invalid_sources_no_null_reference_guess(self):
        r=pack(sample());r['tables']['sourceSets'][0]=[-1]
        with self.assertRaises(ValueError):unpack(r)
        r=pack(sample());r['tables']['results'][0]['stocking']='Forged.esp|00000800'
        with self.assertRaises(ValueError):unpack(r)
    def test_table_expansion_bounded(self):
        r=pack(sample())
        with mock.patch('offline.candidate_store.MAX_EXPANDED',10),self.assertRaisesRegex(ValueError,'expanded-data'):
            unpack(r)
    def test_truncated_duplicate_and_nonfinite_json_rejected(self):
        p=self.root/'c.json'
        for text in ('{"schema":2,','{"schema":2,"schema":1}','{"value":NaN}'):
            p.write_text(text)
            with self.assertRaises(ValueError):read_candidates(p)
    def test_limit_failure_and_nan_keep_previous_complete_file(self):
        p=self.root/'out.json';p.write_bytes(b'old report')
        for r,limit in (({'a':'长'*100},100),({'x':float('nan')},LIMIT)):
            with self.assertRaises(ValueError):stream_json(p,r,limit)
            self.assertEqual(p.read_bytes(),b'old report');self.assertEqual(list(self.root.glob('*.tmp')),[])
    def test_failed_replace_cleans_temporary_keeps_old(self):
        p=self.root/'out.json';p.write_bytes(b'old')
        with mock.patch('offline.candidate_store.os.replace',side_effect=PermissionError('sharing')),self.assertRaises(OSError):
            stream_json(p,{'new':True})
        self.assertEqual(p.read_bytes(),b'old');self.assertEqual(list(self.root.glob('*.tmp')),[])
    def test_same_pair_count_as_user_exceeds_old_limit_but_roundtrips(self):
        r=sample(115,168,1500)
        # Count old pretty UTF-8 bytes without allocating the giant old string.
        old_size=sum(len(s.encode()) for s in json.JSONEncoder(ensure_ascii=False,indent=2).iterencode(r))+1
        self.assertEqual(len(r['entries']),19320);self.assertGreater(old_size,LIMIT)
        p=self.root/'offline-candidates.json';written=write_candidates(p,r)
        self.assertLess(written['bytes'],LIMIT);self.assertLess(written['bytes'],old_size//10)
        loaded=read_candidates(p);self.assertEqual(loaded['entries'],r['entries'])
        self.assertEqual(loaded['_storage']['sha256'],written['sha256'])
        print(f'Synthetic 19320 pairs: old pretty={old_size} bytes, lossless={written["bytes"]} bytes')
    def test_maximum_20000_all_distinct_results(self):
        r=sample(100,200)
        for i,row in enumerate(r['entries']):row.update(Heel=i/10000,rawHeel=i/10000,normalizedResidual=i/200000)
        p=self.root/'max.json';info=write_candidates(p,r)
        self.assertEqual(info['tableCounts']['results'],20000)
        self.assertEqual(read_candidates(p)['entries'],r['entries'])
        print(f'Synthetic 20000 distinct results: {info["bytes"]} bytes')


class IntegrationTests(unittest.TestCase):
    setUp=fixtures.OfflineTests.setUp;tearDown=fixtures.OfflineTests.tearDown
    write=fixtures.OfflineTests.write;fixture=fixtures.OfflineTests.fixture
    def scanner(self):
        self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan();return s
    def test_compute_store_apply_and_modified_source_refused(self):
        s=self.scanner();r=s.recommend('Flat.esp|00000801')
        p=s.output/'offline-candidates.json'
        self.assertEqual(json.loads(p.read_text(encoding='utf-8'))['schema'],2)
        result=apply_report(p,self.root/'dest',[1],expected_sha256=r['_storage']['sha256'])
        self.assertEqual(result['appliedIndexes'],[1])
        with self.assertRaisesRegex(ValueError,'其他窗口'):apply_report(p,self.root/'dest',[0],expected_sha256='not-current')
        (self.data/'High.esp').write_bytes(b'changed')
        with self.assertRaises(ValueError):apply_report(p,self.root/'elsewhere',[1])
    def test_save_failure_retains_calculation_and_retry(self):
        s=self.scanner();p=s.output/'offline-candidates.json';p.write_bytes(b'old')
        with mock.patch('offline.engine.write_candidates',side_effect=PermissionError('locked')):
            with self.assertRaises(CandidateSaveError) as caught:s.recommend('Flat.esp|00000801')
        self.assertEqual(len(caught.exception.report['entries']),2)
        self.assertIs(s.pending_report,caught.exception.report)
        self.assertEqual(p.read_bytes(),b'old')
        write_candidates(p,s.pending_report)
        self.assertEqual(len(read_candidates(p)['entries']),2)


class GroupTests(unittest.TestCase):
    def test_default_is_exact_preview_only(self):
        r=sample();before=canonical(r);g=propose(r)
        self.assertEqual(len(g['groups']),2);self.assertEqual(g['groups'][0]['memberCount'],3)
        self.assertTrue(g['groups'][0]['lossless']);self.assertFalse(g['runtimeReadable'])
        self.assertEqual(canonical(r),before)
    def test_near_values_merge_only_when_requested(self):
        r=sample(3,1)
        for a,v in zip(r['entries'],(1.09,1.1,1.11)):a['Heel']=v
        self.assertEqual(len(propose(r)['groups']),3)
        g=propose(r,.02)['groups'];self.assertEqual(len(g),1);self.assertEqual(g[0]['Heel'],1.1)
        self.assertLessEqual(g[0]['maxCoefficientChange'],.02)
        self.assertAlmostEqual(g[0]['estimatedMaxVertexChange'],.06)
        self.assertIsNone(g[0]['residualAtProposedValue']);self.assertFalse(g[0]['automaticApplicationAllowed'])
    def test_no_transitive_drift(self):
        r=sample(4,1)
        for a,v in zip(r['entries'],(1.0,1.03,1.06,1.09)):a['Heel']=v
        groups=propose(r,.02)['groups'];self.assertGreater(len(groups),1)
        self.assertTrue(all(g['maxCoefficientChange']<=.02 for g in groups))
    def test_different_families_default_separate(self):
        r=sample(2,1);r['entries'][1]['stockingResponse']['family']='other'
        self.assertEqual(len(propose(r)['groups']),2)
        self.assertEqual(len(propose(r,cross_family=True)['groups']),1)
        self.assertFalse(propose(r,cross_family=True)['groups'][0]['sameMeasuredResponse'])
    def test_rejected_and_directions_never_merged_into_safe(self):
        r=sample(5,1);r['entries'][0].update(status='range-saturated');r['entries'][1].update(status='within-mathematical-limits')
        r['entries'][2].update(NoHeel=1.,Heel=0.)
        r['entries'][3].update(NoHeel=1.1,Heel=0.)
        g=propose(r,.02)
        self.assertEqual(g['excludedIndexes'],[0,3]);self.assertEqual(len(g['groups']),3)
    def test_approximate_representative_respects_noninteger_heel_max(self):
        r=sample(2,1);r['heelMax']=1.007
        for row in r['entries']:row['Heel']=1.006
        g=propose(r,.02)['groups'][0]
        self.assertLessEqual(g['Heel'],1.007)
    def test_cannot_pass_review_group_file_to_native_apply(self):
        r=sample();g=propose(r)
        with self.assertRaises(ValueError):unpack(g)

    def test_tolerance_invalid_and_member_scope(self):
        r=sample()
        for value in (float('nan'),-.1,True,.2):
            with self.assertRaises(ValueError):propose(r,value)
        self.assertEqual(propose(r,indexes=[])['groups'],[])
        g=propose(r,indexes=[1]);self.assertEqual(g['groups'][0]['memberIndexes'],[1])

if __name__=='__main__':unittest.main(verbosity=2)
