"""Large-library exports and cross-language native read/cache verification.

Only synthetic plugins and model payloads are created in temporary directories.
VHA_BULK_READER optionally points to the actual compiled C++ worker test runner.
"""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import test_offline as fixtures
from offline.engine import Scanner
from offline.candidate_store import read_candidates, write_candidates
from offline.shoe_groups import propose
from offline import bulk_library as b

class BulkTests(unittest.TestCase):
    setUp=fixtures.OfflineTests.setUp
    tearDown=fixtures.OfflineTests.tearDown
    write=fixtures.OfflineTests.write
    fixture=fixtures.OfflineTests.fixture
    def prepare(self):
        self.fixture();self.dest=self.data/'SKSE/Plugins';self.out=self.root/'out'
        scanner=Scanner(self.data,self.profile,self.out);scanner.scan();scanner.recommend('Flat.esp|00000801')
        self.path=self.out/'offline-candidates.json';self.report=read_candidates(self.path)
        self.library=self.dest/'VanityUBEHeelAdapter/height-library'
        return self.report
    def export(self,ids=None,**kw):
        return b.export_library(self.path,self.dest,list(range(len(self.report['entries']))) if ids is None else ids,**kw)
    def read_shards(self):
        idx=b.decode_index((self.library/'index.vhi').read_bytes())
        return idx,[b.decode_shard((self.library/e['file']).read_bytes()) for e in idx['entries']]
    def native(self,mode='query',row=None,weight=0,allow=1):
        runner=os.environ.get('VHA_BULK_READER')
        if not runner:self.skipTest('native reader path not provided')
        # Windows paths are case-insensitive. Use canonical lowercase files in
        # the Linux fixture; no real game files are modified or emulated here.
        for src in {s['resource']:s for r in self.report['entries'] for s in r.get('sources',[])}.values():
            target=self.data/'meshes'/Path(b.model_key(src['resource']).replace('\\','/'))
            target.parent.mkdir(parents=True,exist_ok=True)
            origin=Path(src['path'])
            if target!=origin:shutil.copyfile(origin,target)
        row=row or self.report['entries'][0]
        args=[runner,mode,str(self.library),str(self.data/'meshes'),row['footwear'],row['footwearAddon'],row['footwearModel'],
              row['stocking'],row['stockingAddon'],row['stockingModel'],str(weight),str(allow)]
        return subprocess.run(args,text=True,capture_output=True,timeout=20)
    def test_export_keeps_user_pairs_and_never_writes_large_overlay(self):
        self.prepare();user=self.dest/'VanityUBEHeelAdapter.user.json';user.parent.mkdir(parents=True,exist_ok=True)
        original=json.dumps({'pairs':[{'stocking':'Sock.esp|00000801','footwear':'High.esp|00000801','NoHeel':0,'Heel':1.1}]}).encode();user.write_bytes(original)
        result=self.export();self.assertEqual(len(result['preservedExistingIndexes']),1);self.assertEqual(len(result['appliedIndexes']),1)
        self.assertEqual(user.read_bytes(),original);idx,shards=self.read_shards();self.assertEqual(len(shards),1)
    def test_10650_pairs_use_150_shards_not_user_array(self):
        r=self.prepare();template=copy.deepcopy(r['entries'][0]);rows=[]
        for si in range(71):
            for fi in range(150):
                a=copy.deepcopy(template);a.update(index=len(rows),stocking=f'Sock.esp|{0x1000+si:08X}',
                    footwear=f'Shoe.esp|{0x1000+fi:08X}',NoHeel=0.,Heel=1.1,status='review-residual',normalizedResidual=.2)
                rows.append(a)
        r['entries']=rows;write_candidates(self.path,r);self.report=r
        result=self.export();self.assertEqual(result['totalMembers'],10650);self.assertEqual(result['shoeShards'],150)
        self.assertEqual(result['warningCount'],10650);self.assertFalse((self.dest/'VanityUBEHeelAdapter.user.json').exists())
        idx,shards=self.read_shards();self.assertEqual(len(idx['stockings']),71)
        self.assertTrue(all(len(s['groups'])==1 and len(s['members'])==71 for s in shards))
        print('Synthetic 10650 library:',len((self.library/'index.vhi').read_bytes()),'index bytes;',sum((self.library/e['file']).stat().st_size for e in idx['entries']),'shard bytes;',len(shards),'shards')
        reader=os.environ.get('VHA_BULK_READER')
        if reader:
            call=subprocess.run([reader,'inspect',str(self.library/'index.vhi')],text=True,capture_output=True,timeout=20)
            self.assertEqual(call.returncode,0,call.stderr);self.assertEqual(call.stdout.strip(),'150 10650 71')
    def test_residual_default_warning_and_strict_skip(self):
        self.prepare();self.report['entries'][0].update(status='review-residual',normalizedResidual=.2)
        write_candidates(self.path,self.report)
        result=self.export([0],allow_residual=False);self.assertFalse(result['published']);self.assertEqual(len(result['skipped']),1)
        result=self.export([0]);self.assertEqual(result['warningCount'],1)
        _,shards=self.read_shards();self.assertEqual(shards[0]['members'][0]['flags']&1,1)
    def test_hard_failure_and_saturation_are_not_warnings(self):
        self.prepare();self.report['entries'][0]['status']='unsupported-topology';self.report['entries'][1]['saturated']=True
        write_candidates(self.path,self.report);result=self.export();self.assertFalse(result['published']);self.assertEqual(len(result['skipped']),2)
    def test_group_values_really_export_explicit_members(self):
        self.prepare();base=copy.deepcopy(self.report['entries'][0]);rows=[]
        for i,v in enumerate((1.09,1.1,1.11)):
            a=copy.deepcopy(base);a.update(index=i,stocking=f'Sock.esp|{0x1000+i:08X}',NoHeel=0.,Heel=v,status='review-residual',normalizedResidual=.2);rows.append(a)
        self.report['entries']=rows;write_candidates(self.path,self.report);self.report=read_candidates(self.path)
        groups=propose(self.report,.02);self.assertEqual(len(groups['groups']),1)
        self.export(group_report=groups,group_indexes=[0]);_,shards=self.read_shards()
        self.assertEqual(shards[0]['groups'],[(0.,1.1)])
        self.assertEqual([m['originalHeel'] for m in shards[0]['members']],[1.09,1.1,1.11])
        self.assertEqual([m['flags'] for m in shards[0]['members']],[3,1,3])
    def test_stale_or_edited_group_report_rejected(self):
        self.prepare();g=propose(self.report);g['sourceCandidateFileSHA256']='wrong'
        with self.assertRaisesRegex(ValueError,'stale'):self.export(group_report=g,group_indexes=[0])
        g=propose(self.report);g['groups'][0]['Heel']=1.5
        with self.assertRaisesRegex(ValueError,'changed'):self.export(group_report=g,group_indexes=[0])
    def test_source_change_keeps_prior_index(self):
        self.prepare();self.export();before=(self.library/'index.vhi').read_bytes()
        src=self.report['entries'][0]['sources'][0];Path(src['path']).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'changed'):self.export()
        self.assertEqual(before,(self.library/'index.vhi').read_bytes())
    def test_concurrent_export_lock_keeps_index(self):
        self.prepare();self.export();before=(self.library/'index.vhi').read_bytes();(self.library/'publish.lock').write_bytes(b'other')
        with self.assertRaises(FileExistsError):self.export()
        self.assertEqual(before,(self.library/'index.vhi').read_bytes());self.assertTrue((self.library/'publish.lock').exists())
    def test_shards_merge_and_only_selected_pair_replaced(self):
        self.prepare();self.export([0]);r=self.export([1]);self.assertEqual(r['totalMembers'],2)
        self.report['entries'][0].update(NoHeel=0.,Heel=.75,status='manual-adjusted');write_candidates(self.path,self.report)
        self.export([0]);_,shards=self.read_shards();self.assertEqual(sum(len(s['members']) for s in shards),2)
        match=next(s for s in shards if s['shoe'][0]==b.ascii_lower(self.report['entries'][0]['footwear']))
        self.assertEqual(match['groups'][match['members'][0]['group']],(0.,.75))
    def test_invalid_values_paths_and_binary_bounds(self):
        self.prepare();self.export();index=(self.library/'index.vhi').read_bytes()
        for data in (index[:7],index+b'x',index[:-1]):
            with self.assertRaises(ValueError):b.decode_index(data)
        for name in ('../x.nif','/x.nif','C:\\x.nif','a\\..\\b.nif','a\0b.nif'):
            with self.assertRaises(ValueError):b.model_key(name)
        idx,ss=self.read_shards();s=ss[0]
        for n,h in ((1.01,0),(0,10.1),(0,math.nan),(.1,.1)):
            bad=copy.deepcopy(s);bad['groups'][0]=(n,h)
            with self.assertRaises(ValueError):b.encode_shard(bad)
        bad=copy.deepcopy(s);bad['members'][0]['sources']=[65536]
        with self.assertRaises(ValueError):b.encode_shard(bad)
    def test_current_mo2_open_first_access_is_preserved(self):
        self.prepare()
        with mock.patch.object(Path,'stat',side_effect=FileNotFoundError('simulated virtual stat')):result=self.export()
        self.assertTrue(result['published'])
    def test_cancelled_export_preserves_active_library(self):
        self.prepare();self.export();before=(self.library/'index.vhi').read_bytes()
        with self.assertRaisesRegex(ValueError,'cancelled'):self.export(cancelled=lambda:True)
        self.assertEqual(before,(self.library/'index.vhi').read_bytes())

    def test_native_decoder_rejects_out_of_range_controls(self):
        runner=os.environ.get('VHA_BULK_READER')
        if not runner:self.skipTest('native reader not provided')
        self.prepare();self.export();idx,ss=self.read_shards()
        payload=bytearray((self.library/idx['entries'][0]['file']).read_bytes())
        reader=b.Reader(payload,b.SHARD_MAGIC);reader.id();reader.d()
        for _ in range(reader.u()):reader.s();reader.q()
        reader.u()
        import struct
        struct.pack_into('<d',payload,reader.pos,1.1)
        path=self.root/'bad.vhs';path.write_bytes(payload)
        result=subprocess.run([runner,'shard',str(path)],text=True,capture_output=True,timeout=10)
        self.assertNotEqual(result.returncode,0);self.assertIn('invalid-library-controls',result.stderr)

    def test_native_reads_published_data_and_caches_without_repeated_io(self):
        self.prepare();self.export();r=self.native('cache');self.assertEqual(r.returncode,0,r.stderr);self.assertTrue(r.stdout.startswith('ready '),r.stdout)
    def test_native_rejects_wrong_weight_and_disabled_warning(self):
        self.prepare();self.report['entries'][0].update(status='review-residual',normalizedResidual=.2);write_candidates(self.path,self.report);self.export()
        r=self.native(weight=10);self.assertTrue(r.stdout.startswith('source-weight-mismatch '),r.stdout+r.stderr)
        r=self.native(allow=0);self.assertTrue(r.stdout.startswith('residual-warning-disabled '),r.stdout+r.stderr)
    def test_native_corrupt_shard_rejected(self):
        self.prepare();self.export();idx,_=self.read_shards();p=self.library/idx['entries'][0]['file'];p.write_bytes(b'wrong')
        r=self.native(row=self.report['entries'][0]);self.assertTrue(r.stdout.startswith('invalid-library '),r.stdout+r.stderr)

if __name__=='__main__':unittest.main()
