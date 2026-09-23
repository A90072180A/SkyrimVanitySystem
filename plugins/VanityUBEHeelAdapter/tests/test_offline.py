"""Offline integration tests with original synthetic TES/NIF/TRI/BSA fixtures."""
import hashlib
import io
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from offline.formats import *
from offline.engine import Scanner,apply_report,atomic_json,validate_user
from offline import geometry as g
from build_command_plugin import payload as command_payload

pack=lambda fmt,*x:struct.pack('<'+fmt,*x)
def subrec(sig,b):return sig.encode()+pack('H',len(b))+b
def record(sig,payload,form=0,flags=0):return pack('4sIIIHHHH',sig.encode(),len(payload),flags,form,0,0,44,0)+payload
def plugin(masters,rows,flags=0):
    header=subrec('HEDR',pack('fII',1.7,len(rows),0x900))
    for m in masters:header+=subrec('MAST',m.encode()+b'\0')+subrec('DATA',bytes(8))
    return record('TES4',header,flags=flags)+b''.join(rows)
def armor(form,addon,name,slots=128,compress=False):
    p=subrec('EDID',name.encode()+b'\0')+subrec('BOD2',pack('II',slots,0))+subrec('MODL',pack('I',addon))
    return record('ARMO',pack('I',len(p))+zlib.compress(p) if compress else p,form,0x40000 if compress else 0)
def addon(form,model,slots=128):
    return record('ARMA',subrec('EDID',b'Addon\0')+subrec('BOD2',pack('II',slots,0))+subrec('MOD3',model.encode()+b'\0')+subrec('DNAM',bytes([0,0,0,2])+bytes(8)),form)
def tf(first):
    zero=pack('3f',0,0,0);rot=pack('9f',1,0,0,0,1,0,0,0,1)
    return (zero+rot if first else rot+zero)+pack('f',1)
def nif(points,triangles,name='Feet',bodytri='!UBE/test/feet.tri'):
    nv,nt=len(points),len(triangles);desc=0x100000000004
    strings=[name,'BODYTRI',bodytri];types=['BSTriShape','NiStringExtraData','NiSkinInstance','NiSkinData','NiSkinPartition']
    shape=pack('IIIII',0,1,1,0xffffffff,14)+tf(True)+pack('I',0xffffffff)+pack('4f',0,0,0,1)
    shape+=pack('IIIQHHII',2,0xffffffff,0xffffffff,desc,0,0,0,0)
    extras=pack('II',1,2);skin=pack('IIII',3,4,0xffffffff,0);skin_data=tf(False)+pack('IB',0,0)
    verts=b''.join(pack('4f',*p,0) for p in points);tris=b''.join(pack('3H',*t) for t in triangles)
    partition=pack('IIIQ',1,len(verts),16,desc)+verts+pack('5H',nv,nt,0,0,4)
    partition+=b'\1'+pack('H'*nv,*range(nv))+b'\0\1'+tris+b'\0\0\0'+pack('Q',desc)+tris
    blocks=[shape,extras,skin,skin_data,partition]
    header=b'Gamebryo File Format, Version 20.2.0.7\n'+pack('IBIII',0x14020007,1,12,5,100)+bytes(3)
    header+=pack('H',5)+b''.join(pack('I',len(t))+t.encode() for t in types)
    header+=pack('5H',*range(5))+pack('5I',*(len(b) for b in blocks))+pack('II',3,max(map(len,strings)))
    header+=b''.join(pack('I',len(s.encode()))+s.encode() for s in strings)+pack('I',0)
    return header+b''.join(blocks)+pack('II',1,0)
def tri(name,morphs):
    def string(n):return pack('B',len(n.encode()))+n.encode()
    b=b'PIRT'+pack('H',1)+string(name)+pack('H',len(morphs))
    for n,offsets in morphs.items():
        b+=string(n)+pack('fH',1.,len(offsets))
        for i,d in offsets.items():b+=pack('H3h',i,*d)
    return b

def bsa(path,resource,payload,version=105,compressed=False):
    folder,filename=resource.rsplit('\\',1);folder=folder.encode()+b'\0';name=filename.encode()+b'\0'
    block=payload
    if compressed:
        # Valid all-literal LZ4 sequence, or zlib for version 104.
        if version==104:block=pack('I',len(payload))+zlib.compress(payload)
        else:
            n=len(payload);ext=bytearray();left=n-15
            if n>=15:
                while left>=255:ext.append(255);left-=255
                ext.append(left)
            block=pack('I',n)+bytes([min(n,15)<<4])+ext+payload
    flags=3|(4 if compressed else 0);fsize=24 if version==105 else 16
    where=36+fsize+1+len(folder)+16+len(name)
    header=b'BSA\0'+pack('8I',version,36,flags,1,1,len(folder),len(name),1)
    record=pack('QIIQ',0,1,0,36+fsize) if version==105 else pack('QII',0,1,36+fsize)
    path.write_bytes(header+record+bytes([len(folder)])+folder+pack('QII',0,len(block),where)+name+block)

class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);self.data=self.root/'Data';self.data.mkdir();self.profile=self.root/'profile';self.profile.mkdir()
    def tearDown(self):self.tmp.cleanup()
    def write(self,name,b):
        p=self.data.joinpath(*name.replace('\\','/').split('/'));p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b);return p
    def fixture(self):
        self.write('Skyrim.esm',plugin([],[],1))
        self.write('Flat.esp',plugin(['Skyrim.esm'],[armor(0x01000801,0x01000800,'Flat'),addon(0x01000800,'!UBE/test/flat_1.nif')]))
        self.write('High.esp',plugin(['Skyrim.esm'],[armor(0x01000801,0x01000800,'High',compress=True),addon(0x01000800,'!UBE/test/high_1.nif')]))
        self.write('Sock.esp',plugin(['Skyrim.esm'],[armor(0x01000801,0x01000800,'Stocking',1<<18),addon(0x01000800,'!UBE/test/sock_1.nif',1<<18)]))
        (self.profile/'plugins.txt').write_text('*Flat.esp\n*High.esp\n*Sock.esp\nInactive.esp\n')
        (self.profile/'loadorder.txt').write_text('Skyrim.esm\nFlat.esp\nHigh.esp\nSock.esp\nInactive.esp\n')
        points=[(x/10,y/10,0.) for y in range(15) for x in range(20)];triangles=[]
        for y in range(14):
            for x in range(19):
                i=y*20+x;triangles.extend([(i,i+1,i+20),(i+1,i+21,i+20)])
        for n,z,name in [('flat',0,'Feet'),('high',2,'Feet'),('sock',1,'Sock')]:
            b=nif([(p[0],p[1],z) for p in points],triangles,name,'!UBE/test/'+n+'.tri')
            self.write(f'meshes/!UBE/test/{n}_0.nif',b);self.write(f'meshes/!UBE/test/{n}_1.nif',b)
            self.write(f'meshes/!UBE/test/{n}.tri',tri(name,{'NoHeel':{i:(0,0,-1) for i in range(300)},'Heel':{i:(0,0,1) for i in range(300)}} if n=='sock' else {}))
    def test_full_scan_pair_and_apply(self):
        self.fixture();s=Scanner(self.data,self.profile,self.root/'out');catalog=s.scan()
        self.assertEqual(len(s.plugins),4);self.assertEqual(sum(r['status']=='measurable' for r in s.rows),3)
        r=s.recommend('Flat.esp|00000801');self.assertEqual(len(r['entries']),2)
        by={p['footwear']:p for p in r['entries']}
        self.assertEqual(by['Flat.esp|00000801']['NoHeel'],1.);self.assertEqual(by['High.esp|00000801']['Heel'],1.)
        self.assertTrue(all(p['status']=='within-mathematical-limits' for p in r['entries']))
        dest=self.root/'plugins';dest.mkdir();atomic_json(dest/'VanityUBEHeelAdapter.json',{'heelMax':2})
        receipt=apply_report(s.output/'offline-candidates.json',dest,[0,1]);self.assertEqual(len(receipt['appliedIndexes']),2)
        receipt=apply_report(s.output/'offline-candidates.json',dest,[0,1]);self.assertEqual(len(receipt['preservedExistingIndexes']),2)
        user=json.loads((dest/'VanityUBEHeelAdapter.user.json').read_text());validate_user(user);self.assertEqual(len(user['items']),1)
    def test_unlabelled_body_slot_stocking_is_scanned(self):
        self.fixture()
        self.write('Sock.esp',plugin(['Skyrim.esm'],[armor(0x01000801,0x01000800,'CPB_SB',1<<2),addon(0x01000800,'!UBE/test/sock_1.nif',1<<2)]))
        scanner=Scanner(self.data,self.profile,self.root/'out');scanner.scan()
        row=next(r for r in scanner.rows if r['armor']=='Sock.esp|00000801')
        self.assertEqual((row['kind'],row['status']),('stocking','measurable'))
    def test_winning_patch_and_deleted(self):
        self.fixture()
        self.write('Patch.esp',plugin(['Skyrim.esm','High.esp'],[armor(0x01000801,0x02000800,'Patched High'),addon(0x02000800,'!UBE/test/high_1.nif')]))
        with (self.profile/'plugins.txt').open('a') as f:f.write('*Patch.esp\n')
        recs,src=load_records(self.data,active_plugins(self.data,self.profile))
        r=recs['high.esp|00000801'];self.assertEqual(r['owner'],'Patch.esp');self.assertEqual(r['addons'],['Patch.esp|00000800'])
        self.write('Patch.esp',plugin(['Skyrim.esm','High.esp'],[record('ARMO',b'',0x01000801,0x20)]))
        recs,_=load_records(self.data,active_plugins(self.data,self.profile));self.assertTrue(recs['high.esp|00000801']['deleted'])
    def test_missing_master_and_inactive(self):
        self.fixture();(self.profile/'plugins.txt').write_text('*High.esp\n')
        self.assertEqual(active_plugins(self.data,self.profile),['Skyrim.esm','High.esp'])
        self.write('High.esp',plugin(['Missing.esm'],[]))
        with self.assertRaises(FormatError):load_records(self.data,active_plugins(self.data,self.profile))
    def test_malformed_plugin_subrecords(self):
        with self.assertRaises(FormatError):list(subrecords(b'XXXX'+pack('H',3)+b'abc'))
        self.assertEqual(list(subrecords(b'XXXX'+pack('HI',4,8)+b'TEST'+pack('H',0)+b'abcdefgh')),[('TEST',b'abcdefgh')])
        with self.assertRaises(FormatError):decompress_zlib(zlib.compress(b'ab'),1)
        with self.assertRaises(FormatError):decompress_zlib(zlib.compress(b'ab')+b'junk',2)
    def test_malformed_nif_tri(self):
        b=nif([(0,0,0),(1,0,0),(0,1,0)],[(0,1,2)])
        s=parse_nif(b);self.assertEqual(s[0]['status'],'complete')
        for i in range(0,len(b),7):
            with self.subTest(i=i),self.assertRaises((ValueError,struct.error)):parse_nif(b[:i])
        with self.assertRaises(ValueError):parse_nif(b+b'x')
        data=tri('Feet',{'Heel':{0:(1,2,3)}});self.assertEqual(parse_tri(data)[0]['Feet']['Heel'][0],(1,2,3))
        for i in range(len(data)):
            with self.subTest(i=i),self.assertRaises((ValueError,struct.error)):parse_tri(data[:i])
    def test_archives_compression_and_loose_priority(self):
        payload=b'12345678901234567890'*5
        for version in (104,105):
            for compressed in (False,True):
                p=self.data/f'test{version}{compressed}.bsa';bsa(p,'meshes\\!UBE\\test\\test.nif',payload,version,compressed)
                archive=Archive(p);self.assertEqual(archive.read('meshes\\!ube\\test\\test.nif'),payload)
                r=Resources(self.data,[p]);self.assertEqual(r.read('!UBE/test/test.nif')[0],payload)
        self.write('meshes/!UBE/test/test.nif',b'loose');self.assertEqual(r.read('!UBE/test/test.nif')[0],b'loose')
        with self.assertRaises(FormatError):lz4_block(b'\x00\x00\x00',4)
        for path in ('../secret','C:\\foo.nif','\\\\server\\foo.nif','foo:bar'):
            with self.assertRaises(FormatError):relative_model(path)
    def test_surface_projection_and_subset(self):
        p=[(0,0,0),(1,0,0),(0,1,0)];s=g.Surface(p,[(0,1,2)])
        d,t,w=s.nearest((.2,.3,1));self.assertAlmostEqual(d,1);self.assertAlmostEqual(sum(w),1)
        base=p*10;tris=[(i,i+1,i+2) for i in range(0,30,3)]
        target=base[:27]
        with self.assertRaises(FormatError):g.align(base,tris,target,tris[:-1])
        unique=[(0,i,i+1) for i in range(1,26)]+[(27,28,29)]
        a,common,meta=g.align(base,unique,target,unique[:-1]);self.assertEqual(meta['removedVertices'],3)
        # Equal disconnected components make the correspondence ambiguous.
        # The exact triangles alone don't distinguish this fixture's islands.
    def test_range_and_invalid_config(self):
        for user in ({'unknown':1},{'pairs':[{'stocking':'Sock.esp|00000001','footwear':'Shoe.esp|00000002','NoHeel':1.1,'Heel':0}]},{'settings':{'heelMax':False}}):
            with self.assertRaises(ValueError):validate_user(user)
    def test_changed_source_never_applies(self):
        self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan();s.recommend('Flat.esp|00000801')
        self.write('meshes/!UBE/test/high_1.nif',b'changed');dest=self.root/'plugins'
        with self.assertRaises(ValueError):apply_report(s.output/'offline-candidates.json',dest,[1])
        self.assertFalse((dest/'VanityUBEHeelAdapter.user.json').exists())
    def test_keep_existing_manual(self):
        self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan();s.recommend('Flat.esp|00000801')
        dest=self.root/'plugins';atomic_json(dest/'VanityUBEHeelAdapter.user.json',{'pairs':[{'stocking':'Sock.esp|00000801','footwear':'High.esp|00000801','NoHeel':0,'Heel':1.1}]})
        apply_report(s.output/'offline-candidates.json',dest,[0,1]);j=json.loads((dest/'VanityUBEHeelAdapter.user.json').read_text())
        self.assertEqual(next(v for v in j['pairs'] if v['footwear']=='High.esp|00000801')['Heel'],1.1)
        self.assertTrue((dest/'VanityUBEHeelAdapter.user.json.bak').exists())
    def test_command_esp_structure(self):
        b=command_payload();size,flags=struct.unpack_from('<II',b,4);self.assertEqual(flags,0x200)
        h=dict(subrecords(b[24:24+size]));self.assertEqual(h['MAST'],b'Skyrim.esm\0')
        p=24+size;self.assertEqual(b[p:p+4],b'GRUP');self.assertEqual(struct.unpack_from('<I',b,p+4)[0],len(b)-p)
        p+=24;self.assertEqual(b[p:p+4],b'GLOB');self.assertEqual(struct.unpack_from('<I',b,p+12)[0],0x01000800)
        f=dict(subrecords(b[p+24:]));self.assertEqual(f['EDID'],b'VHA_Reload\0');self.assertEqual(f['FNAM'],b'f');self.assertEqual(struct.unpack('<f',f['FLTV'])[0],0.)

if __name__=='__main__':unittest.main(verbosity=2)
