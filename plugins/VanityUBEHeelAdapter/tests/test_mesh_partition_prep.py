"""Synthetic reversible stock-foot face partition tests. No user model bytes."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from offline.formats import parse_nif, parse_tri, FormatError
from offline import mesh_partition_prep as m
from test_offline import nif, tri, tf, pack

def fixture():
    points=[(0.,0.,0.),(1.,0.,0.),(0.,1.,0.),(1.,1.,0.),(0.,2.,0.),(1.,2.,0.)]
    faces=[(0,1,2),(1,3,2),(2,3,4),(3,5,4)]
    raw=nif(points,faces,'Stocking','!UBE/stock.tri')
    c=m.unpack_container(raw)
    c['types'].append('NiNode');c['typeraw']=pack('H',len(c['types']))+b''.join(pack('I',len(x))+x.encode() for x in c['types'])
    name=len(c['strings']);c['strings'].append(b'Root')
    node=pack('4I',name,0,0xffffffff,14)+tf(True)+pack('4I',0xffffffff,1,0,0)
    root=len(c['blocks']);c['blocks'].append(node);c['ids'].append(len(c['types'])-1);c['footer']=pack('II',1,root)
    return m.pack_container(c), tri('Stocking',{'NoHeel':{0:(0,0,1),3:(1,0,0)},'Heel':{0:(0,0,-1)},'Body':{5:(0,1,0)}})

class PreparationTests(unittest.TestCase):
    def setUp(self):self.nif,self.tri=fixture()
    def prepare(self,faces=None,**kw):
        return m.split(self.nif,self.tri,[0,1] if faces is None else faces,source='Stocking',new='Foot',tri_resource='!UBE/VHA/test-prepared.tri',**kw)
    def test_roundtrip_container(self):self.assertEqual(m.pack_container(m.unpack_container(self.nif)),self.nif)
    def test_exact_face_partition(self):
        raw,tr,report=self.prepare();ss=parse_nif(raw);old=parse_nif(self.nif)[0]
        self.assertEqual(len(ss),2)
        self.assertEqual(ss[0]['triangles'],old['triangles'][2:]);self.assertEqual(ss[1]['triangles'],old['triangles'][:2])
        self.assertEqual(ss[0]['points'],old['points']);self.assertEqual(ss[1]['points'],old['points'])
        self.assertTrue(report['fullVisibleUnionEqualsOriginal']);self.assertFalse(report['runtimeVisibilityImplemented'])
    def test_new_bodytri_path(self):
        raw,_,r=self.prepare()
        self.assertTrue(all(s['bodyTris']==['!UBE\\VHA\\test-prepared.tri'] for s in parse_nif(raw)))
    def test_original_and_new_morphs(self):
        _,tr,_=self.prepare();before,_=parse_tri(self.tri);after,_=parse_tri(tr)
        self.assertEqual(after['Stocking'],before['Stocking']);self.assertEqual(after['Foot'],before['Stocking'])
    def test_source_not_mutated(self):
        old=(self.nif,self.tri);self.prepare();self.assertEqual(old,(self.nif,self.tri))
    def test_duplicate_faces_rejected(self):
        with self.assertRaises(FormatError):self.prepare([0,0])
    def test_empty_faces_rejected(self):
        with self.assertRaises(FormatError):self.prepare([])
    def test_whole_shape_rejected(self):
        with self.assertRaises(FormatError):self.prepare([0,1,2,3])
    def test_bad_face_types_and_bounds(self):
        for ids in ([True],[.5],[-1],[4]):
            with self.subTest(ids=ids),self.assertRaises(FormatError):self.prepare(ids)
    def test_unsafe_output_tri(self):
        for path in ('../test.tri','C:/test.tri','!UBE/test.nif','!UBE/stock.tri'):
            with self.subTest(path=path),self.assertRaises(FormatError):
                m.split(self.nif,self.tri,[0],tri_resource=path,source='Stocking',new='Foot')
    def test_repeat_preparation_rejected(self):
        raw,tr,_=self.prepare()
        with self.assertRaises(FormatError):m.split(raw,tr,[0],tri_resource='!UBE/new2.tri',source='Stocking',new='Foot')
    def test_missing_tri_morph_shape(self):
        with self.assertRaises(FormatError):m.split(self.nif,tri('Other',{}),[0],source='Stocking',new='Foot',tri_resource='!UBE/new.tri')
    def test_original_subtrees_and_skin_data_preserved(self):
        raw,_,_=self.prepare();old=m.unpack_container(self.nif);new=m.unpack_container(raw)
        self.assertEqual(old['blocks'][3],new['blocks'][3])
        self.assertEqual(old['blocks'][0],new['blocks'][0])
        self.assertEqual(old['blocks'][2],new['blocks'][2])
    def test_full_visible_all_height_states_equivalent(self):
        raw,tr,_=self.prepare();pts=parse_nif(raw)[0]['points'];mm,_=parse_tri(tr)
        for nh,hh in ((0,0),(1,0),(.5,0),(0,.47),(0,1.1),(0,2)):
            for i,p in enumerate(pts):
                states=[]
                for name in ('Stocking','Foot'):
                    n=mm[name]['NoHeel'].get(i,(0,0,0));h=mm[name]['Heel'].get(i,(0,0,0));b=mm[name]['Body'].get(i,(0,0,0))
                    states.append(tuple(p[k]+nh*n[k]+hh*h[k]+.38*b[k] for k in range(3)))
                self.assertEqual(states[0],states[1])
    def test_truncated_inputs(self):
        for n in (0,20,len(self.nif)-1):
            with self.subTest(length=n),self.assertRaises(FormatError):m.split(self.nif[:n],self.tri,[0],source='Stocking',new='Foot',tri_resource='!UBE/new.tri')

if __name__=='__main__':unittest.main()
