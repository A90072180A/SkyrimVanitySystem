"""Portable synthetic regressions. Private NIF/TRI data is never committed."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from offline.formats import parse_partition, parse_nif, FormatError
from offline import foot_reference as fr
from offline.feet_actions import rescan_keys, witchy_cpb_trial
from offline.engine import Scanner, VERSION
import test_offline as fixtures
from test_offline import nif, pack


def partition(parts, nv=6, *, descriptor=0x100000000004):
    verts=b''.join(pack('4f',i,i/2,0,0) for i in range(nv))
    out=pack('IIIQ',len(parts),len(verts),16,descriptor)+verts
    for spec in parts:
        mapping,triangles=spec['map'],spec['faces']
        count=spec.get('count',len(mapping) if mapping is not None else nv)
        raw=b''.join(pack('3H',*t) for t in triangles)
        out+=pack('5H',count,len(triangles),0,0,4)
        out+=b'\0' if mapping is None else b'\1'+pack('H'*len(mapping),*mapping)
        out+=b'\0\1'+raw+b'\0'+pack('BBQ',spec.get('lod',0),0,descriptor)
        out+=b''.join(pack('3H',*t) for t in spec.get('copy',triangles))
    return out


class PartitionTests(unittest.TestCase):
    def test_global_multi_keeps_domain_and_face_order(self):
        p,t=parse_partition(partition([{'map':[0,1,3],'faces':[(0,1,3)]},
                                     {'map':[2,4,5],'faces':[(2,4,5)]}]))
        self.assertEqual(len(p),6);self.assertEqual(t,[(0,1,3),(2,4,5)])
        self.assertEqual(p[4],(4.,2.,0.))

    def test_sparse_skin_map_does_not_repack_unused_vertices(self):
        p,t=parse_partition(partition([{'map':[1,3,5],'faces':[(1,3,5)]}]))
        self.assertEqual(len(p),6);self.assertEqual(t,[(1,3,5)])

    def test_identity_without_map(self):
        self.assertEqual(parse_partition(partition([{'map':None,'faces':[(0,1,5)]}]))[1],[(0,1,5)])

    def test_subset_requires_map(self):
        with self.assertRaisesRegex(FormatError,'missing-subset'):
            parse_partition(partition([{'map':None,'count':3,'faces':[(0,1,2)]}]))

    def test_duplicate_map_rejected(self):
        with self.assertRaisesRegex(FormatError,'vertex-map'):
            parse_partition(partition([{'map':[0,0,1],'faces':[(0,1,0)]}]))

    def test_map_outside_buffer_rejected(self):
        with self.assertRaisesRegex(FormatError,'vertex-map'):
            parse_partition(partition([{'map':[0,1,6],'faces':[(0,1,2)]}]))

    def test_triangle_outside_map_rejected(self):
        with self.assertRaisesRegex(FormatError,'outside-partition-map'):
            parse_partition(partition([{'map':[0,1,3],'faces':[(0,1,2)]}]))

    def test_triangle_outside_buffer_rejected(self):
        with self.assertRaisesRegex(FormatError,'out-of-bounds'):
            parse_partition(partition([{'map':None,'faces':[(0,1,6)]}]))

    def test_copy_disagreement_rejected(self):
        with self.assertRaisesRegex(FormatError,'triangle-domain-mismatch'):
            parse_partition(partition([{'map':None,'faces':[(0,1,2)],'copy':[(1,0,2)]}]))

    def test_overlap_not_silently_duplicated_or_removed(self):
        with self.assertRaisesRegex(FormatError,'overlapping-partition-faces'):
            parse_partition(partition([{'map':None,'faces':[(0,1,2)]},
                                       {'map':None,'faces':[(2,1,0)]}]))

    def test_lod_rejected(self):
        with self.assertRaisesRegex(FormatError,'partition-lod'):
            parse_partition(partition([{'map':None,'faces':[(0,1,2)],'lod':1}]))

    def test_truncated_multi_all_prefixes_rejected(self):
        raw=partition([{'map':[0,1,3],'faces':[(0,1,3)]},
                       {'map':[2,4,5],'faces':[(2,4,5)]}])
        for i in range(len(raw)):
            with self.subTest(prefix=i),self.assertRaises((ValueError,struct.error)):
                parse_partition(raw[:i])

    def test_trailing_bytes_rejected(self):
        with self.assertRaisesRegex(FormatError,'trailing-data'):
            parse_partition(partition([{'map':None,'faces':[(0,1,2)]}])+b'x')

    def test_partition_limit(self):
        for count in (0,4097,0xffffffff):
            with self.subTest(count=count),self.assertRaisesRegex(FormatError,'partition-count-limit'):
                parse_partition(pack('I',count))


class ReferenceTests(unittest.TestCase):
    def shape(self,name='FeetH'):
        return parse_nif(nif([(0.,0.,0.),(1.,0.,0.),(0.,1.,0.)],[(0,1,2)],name))[0]

    def gate(self, shape):
        return mock.patch.multiple(fr,REFERENCE_VERTICES=len(shape['points']),
            REFERENCE_TRIANGLES=len(shape['triangles']),REFERENCE_TOPOLOGY=fr.topology_digest(shape))

    def test_known_alias_requires_full_digest(self):
        s=self.shape()
        self.assertEqual(fr.select_reference([s])[0],[])
        with self.gate(s):
            chosen,evidence=fr.select_reference([s]);self.assertEqual(chosen,[s])
            self.assertEqual(evidence['method'],'validated-alias')
            self.assertEqual(s['name'],'FeetH')

    def test_triangle_permutation_not_accepted_from_count(self):
        s=self.shape()
        with self.gate(s):
            s['triangles']=[(2,1,0)]
            self.assertEqual(fr.select_reference([s])[0],[])

    def test_transforms_must_match(self):
        for key in ('skin','local','parentTransforms'):
            s=self.shape()
            with self.gate(s):
                t=copy.deepcopy(fr.IDENTITY);t['scale']=2.
                s[key]=[t] if key=='parentTransforms' else t
                self.assertEqual(fr.select_reference([s])[0],[])

    def test_zero_parent_is_equivalent(self):
        s=self.shape()
        with self.gate(s):
            s['parentTransforms']=[fr.IDENTITY,copy.deepcopy(fr.IDENTITY)]
            self.assertEqual(fr.select_reference([s])[0],[s])

    def test_alias_ambiguous_even_if_one_unreadable(self):
        s=self.shape();other=self.shape('HeelFeet');other['status']='broken'
        with self.gate(s):
            self.assertEqual(fr.select_reference([s,other])[0],[])

    def test_shell_never_used_as_foot(self):
        s=self.shape('Shoes')
        with self.gate(s):
            self.assertEqual(fr.select_reference([s])[1]['reason'],'no-reference-feet')

    def test_classic_subset_unchanged(self):
        s=self.shape('fEeT');self.assertEqual(fr.select_reference([s])[0],[s])

    def test_bad_classic_not_replaced_with_alias(self):
        s=self.shape();classic=self.shape('Feet');classic['status']='broken'
        with self.gate(s):self.assertEqual(fr.select_reference([classic,s])[0],[])

    def test_nonzero_additional_preset_refused(self):
        s=self.shape()
        with self.gate(s):
            self.assertEqual(fr.select_reference([s],{'BiggerFeet':.1})[0],[])
            self.assertEqual(fr.select_reference([s],{'BiggerFeet':0})[0],[s])

    def test_no_reference_error_is_not_parser_error(self):
        s=self.shape('Shoes')
        self.assertEqual(fr.failure_status(fr.select_reference([s])[1]),'no-reference-feet')
        s['status']='unsupported-strips'
        self.assertEqual(fr.failure_status(fr.select_reference([s])[1]),'geometry-read-error-no-reference')


class WorkflowTests(unittest.TestCase):
    def fixture(self):
        helper=fixtures.OfflineTests();helper.setUp();self.addCleanup(helper.tearDown);helper.fixture();return helper

    def test_scan_alias_get_shape_recommend_apply_library(self):
        h=self.fixture()
        for weight in (0,1):
            p=h.data/f'meshes/!UBE/test/high_{weight}.nif';old=parse_nif(p.read_bytes())[0]
            p.write_bytes(nif(old['points'],old['triangles'],'FeetH','!UBE/test/high.tri'))
        with ReferenceTests().gate(old):
            scanner=Scanner(h.data,h.profile,h.root/'out');catalog=scanner.scan()
            row=next(r for r in scanner.rows if r['armor']=='High.esp|00000801')
            self.assertEqual(row['status'],'measurable');self.assertEqual(row['geometry'],'FeetH')
            self.assertEqual(catalog['toolVersion'],fr.TOOL_VERSION);self.assertEqual(catalog['generatorVersion'],VERSION)
            report=scanner.recommend('Flat.esp|00000801',shoe_keys=[row['key']])
            self.assertEqual(len(report['entries']),1);self.assertEqual(report['entries'][0]['Heel'],1.)
            from offline.bulk_library import export_library,decode_index,decode_shard
            receipt=export_library(scanner.output/'offline-candidates.json',h.root/'plugins',[0])
            self.assertEqual(receipt['appliedIndexes'],[0])
            root=Path(receipt['library']);idx=decode_index((root/'index.vhi').read_bytes())
            shard=decode_shard((root/idx['entries'][0]['file']).read_bytes())
            self.assertEqual(shard['members'][0]['group'],0)

    def test_targeted_scan_never_opens_unselected_mesh(self):
        h=self.fixture();scanner=Scanner(h.data,h.profile,h.root/'out')
        key='High.esp|00000801::High.esp|00000800'
        with mock.patch.object(scanner,'resource',wraps=scanner.resource) as reader:
            r=scanner.scan(keys=[key]);opened=[str(x.args[0]).lower() for x in reader.call_args_list]
        self.assertEqual([x['key'] for x in r['entries']],[key])
        self.assertFalse(any('sock' in x or 'flat' in x for x in opened))
        self.assertEqual(r['targetedScan']['missingKeys'],[])

    def test_targeted_missing_keys_and_empty_rejected(self):
        h=self.fixture();scanner=Scanner(h.data,h.profile,h.root/'out')
        with self.assertRaises(ValueError):scanner.scan(keys=[])
        r=scanner.scan(keys=['Gone.esp|00000801::Gone.esp|00000800'])
        self.assertEqual(len(r['targetedScan']['missingKeys']),1)

    def test_rescan_requires_socks_and_unique_anchor(self):
        rows=[{'key':'s','armor':'Sock','kind':'stocking','status':'measurable'},
              {'key':'a','armor':'Anchor','kind':'footwear','status':'measurable','UBE':True},
              {'key':'x','armor':'Bad','kind':'footwear','status':'no-usable-Feet','UBE':True},
              {'key':'ok','armor':'Other','kind':'footwear','status':'measurable','UBE':True}]
        self.assertEqual(rescan_keys(rows,['s'],'Anchor'),['a','s','x'])
        with self.assertRaises(ValueError):rescan_keys(rows,[],'Anchor')
        with self.assertRaises(ValueError):rescan_keys(rows,['s'],'Gone')

    def test_trial_is_explicit_unmeasured_and_has_no_approval(self):
        sock={'UBE':True,'armor':'[Caenarvon] Cosplay Basics.esp|00000D1A',
              'addon':'[Caenarvon] Cosplay Basics UBE patch.esp|0000093B',
              'model':'!UBE\\Caenarvon\\Cosplay\\cpb_sb_1.nif'}
        shoe={'UBE':True,'armor':'Witchy Agata Heels.esp|00000800',
              'addon':'Witchy Agata Heels UBE patch.esp|00000801',
              'model':'!UBE\\[Spaz490]\\Witchy Agata Heels\\witchy_1.nif'}
        seed=witchy_cpb_trial(sock,shoe)
        self.assertEqual(seed['Heel'],'0.47');self.assertIn('UNVALIDATED',seed['note'])
        self.assertNotIn('approval',seed)
        with self.assertRaises(ValueError):witchy_cpb_trial(sock,{**shoe,'armor':'Witchy Agata Heels.esp|00000802'})
        with self.assertRaises(ValueError):witchy_cpb_trial({**sock,'model':'Other.nif'},shoe)


if __name__=='__main__':
    unittest.main(verbosity=2)
