"""Development-only reversible face grouping; never installs or changes source files.

Keeps complete vertex domains in both shapes so UVs, packed skin weights and all
TRI vertex indices are preserved. This costs duplicated buffers, deliberately.
The generated full-visible model is not a runtime clipping fix.
"""
from __future__ import annotations
import struct, hashlib
from .formats import Reader, FormatError, text, parse_nif, parse_tri, parse_partition, transform, relative_model
MAX=64*1024*1024

def require(ok,msg):
 if not ok:raise FormatError(msg)
def hash_(b):return hashlib.sha256(b).hexdigest()
def unpack_container(data):
 require(len(data)<=MAX,'size-limit');r=Reader(data);header=b''
 while len(header)<128:
  header+=r.take(1)
  if header[-1:]==b'\n':break
 require(header==b'Gamebryo File Format, Version 20.2.0.7\n','unsupported-header')
 require((r.u32(),r.u8(),r.u32())==(0x14020007,1,12),'unsupported-version')
 prefix=data[:r.pos];n=r.count(4096);start=r.pos;require(r.u32()==100,'unsupported-stream')
 for _ in range(3):r.take(r.u8())
 stream=data[start:r.pos];start=r.pos;types=[text(r.take(r.count(65536))) for _ in range(r.u16())];typeraw=data[start:r.pos]
 ids=list(r.unpack('H'*n));sizes=r.unpack('I'*n);sn=r.count(65536);r.u32();strings=[r.take(r.count(65536)) for _ in range(sn)];start=r.pos
 for _ in range(r.count(4096)):r.u32()
 groups=data[start:r.pos];bs=[r.take(size) for size in sizes];start=r.pos;roots=[r.u32() for _ in range(r.count(4096))];r.done()
 require(all(i<len(types) for i in ids),'type-index');require(all(i<n for i in roots),'root-index')
 return dict(prefix=prefix,stream=stream,typeraw=typeraw,types=types,ids=ids,strings=strings,groups=groups,blocks=bs,footer=data[start:])
def pack_container(c):
 blocks=c['blocks'];strings=c['strings'];ids=c['ids'];n=len(blocks)
 return b''.join([c['prefix'],struct.pack('<I',n),c['stream'],c['typeraw'],struct.pack('<'+'H'*n,*ids),struct.pack('<'+'I'*n,*map(len,blocks)),struct.pack('<II',len(strings),max(map(len,strings),default=0)),*(struct.pack('<I',len(s))+s for s in strings),c['groups'],*blocks,c['footer']])
def net(r):
 name=r.u32()
 for _ in range(r.count(4096)):r.u32()
 r.u32();return name

def partition_faces(raw):
 # Fully validated identical-domain SSE partitions only. Refuse remap/strip cases.
 points,tris=parse_partition(raw);r=Reader(raw);require(r.u32()==1,'multiple-partitions')
 size,stride=r.u32(),r.u32();r.u64();r.take(size);nv=r.u16();ntoff=r.pos;nt=r.u16();nb,ns,nw=r.unpack('3H');r.take(nb*2)
 if r.boolean():r.take(nv*2)
 if r.boolean():r.take(nv*nw*4)
 require(r.boolean(),'no-faces');a=r.pos;r.take(nt*6)
 if r.boolean():r.take(nv*nw)
 r.take(2);r.u64();b=r.pos;r.take(nt*6);r.done()
 return tris,ntoff,a,b

def subset_partition(raw,faces):
 tris,ntoff,a,b=partition_faces(raw)
 require(faces and len(faces)<len(tris),'empty-or-whole-subset')
 require(faces==sorted(set(faces)) and 0<=faces[0] and faces[-1]<len(tris),'invalid-face-list')
 payload=b''.join(struct.pack('<3H',*tris[i]) for i in faces);nt=len(tris)
 out=bytearray(raw[:a]+payload+raw[a+nt*6:b]+payload+raw[b+nt*6:]);struct.pack_into('<H',out,ntoff,len(faces))
 p,t=parse_partition(bytes(out));require(t==[tris[i] for i in faces],'roundtrip-faces');return bytes(out)

def duplicate_tri_shape(raw,old,new):
 require(0<len(new.encode())<=255 and new!=old and '\0' not in new,'invalid-new-name');r=Reader(raw);require(r.take(4)==b'PIRT','unsupported-TRI');out=b'PIRT';found=False
 for dim in (3,2):
  if not r.left:break
  count=r.u16();records=[];copy=None
  for _ in range(count):
   start=r.pos;name=r.name();nameend=r.pos;nm=r.u16()
   for _ in range(nm):r.name();r.f32();cnt=r.u16();r.take(cnt*(2+2*dim))
   chunk=raw[start:r.pos];records.append(chunk)
   require(name!=new,'new-TRI-name-already-exists')
   if name==old:copy=bytes([len(new.encode())])+new.encode()+raw[nameend:r.pos]
  if copy is not None:records.append(copy);found=True
  out+=struct.pack('<H',len(records))+b''.join(records)
 r.done();require(found,'source-TRI-shape-absent');before,inv=parse_tri(raw);after,inv2=parse_tri(out)
 require(after[old]==before[old] and after[new]==before[old],'TRI-copy-mismatch')
 for name in before:require(after[name]==before[name],'other-TRI-data-changed')
 return out

def split(data, tri, foot_faces, *, tri_resource, source='SB_bodystocking', new='VHA_CPB_Foot'):
 tri_resource = relative_model(tri_resource)
 require(tri_resource.lower().endswith('.tri'), 'output-resource-must-be-TRI')
 require(isinstance(foot_faces, (list, tuple)) and all(type(i) is int for i in foot_faces), 'invalid-face-list')
 input_shapes = parse_nif(data)
 require(len(input_shapes)==1 and input_shapes[0]['status']=='complete', 'only-one-source-geometry-supported')
 require(len(input_shapes[0]['bodyTris'])==1, 'source-requires-one-BODYTRI')
 source_resource = input_shapes[0]['bodyTris'][0]
 require(tri_resource.replace('/', '\\').casefold()!=source_resource.replace('/', '\\').casefold(), 'prepared-TRI-requires-new-resource-path')
 c=unpack_container(data);bs=c['blocks'];names=[text(s) for s in c['strings']]
 require(new not in names,'already-prepared')
 resource_strings=[i for i,v in enumerate(names) if v.replace('/', '\\').casefold()==source_resource.replace('/', '\\').casefold()]
 require(len(resource_strings)==1, 'ambiguous-BODYTRI-string')
 c['strings'][resource_strings[0]]=tri_resource.encode('utf-8')
 kinds=[c['types'][i] for i in c['ids']]
 shapes=[i for i,k in enumerate(kinds) if k=='BSTriShape' and names[struct.unpack_from('<I',bs[i])[0]]==source]
 require(len(shapes)==1,'ambiguous-source-shape');sid=shapes[0];br=Reader(bs[sid]);net(br);br.u32();transform(br);br.u32();br.take(16);skin_offset=br.pos;skin=br.u32()
 require(kinds[skin] in ('NiSkinInstance','BSDismemberSkinInstance'),'unsupported-skin')
 skin_data,part=struct.unpack_from('<II',bs[skin]);require(kinds[part]=='NiSkinPartition','bad-partition')
 require(sum(1 for i,k in enumerate(kinds) if k in ('NiSkinInstance','BSDismemberSkinInstance') and struct.unpack_from('<II',bs[i])[1]==part)==1,'shared-input-partition')
 original_faces,_,_,_=partition_faces(bs[part]);foot_faces=sorted(foot_faces);footset=set(foot_faces);body=[i for i in range(len(original_faces)) if i not in footset]
 parents=[]
 for i,k in enumerate(kinds):
  if k!='NiNode':continue
  rr=Reader(bs[i]);net(rr);rr.u32();transform(rr);rr.u32();at=rr.pos;count=rr.count(4096);children=list(rr.unpack('I'*count))
  if sid in children:parents.append((i,at,count,children,rr.pos))
 require(len(parents)==1,'ambiguous-parent');parent,at,count,children,end=parents[0]
 require(count<4096,'too-many-children');part2=len(bs);skin2=part2+1;shape2=part2+2
 orig_part=bs[part];bs[part]=subset_partition(orig_part,body);bs.append(subset_partition(orig_part,foot_faces));c['ids'].append(c['ids'][part])
 sk=bytearray(bs[skin]);struct.pack_into('<I',sk,4,part2);bs.append(bytes(sk));c['ids'].append(c['ids'][skin])
 shape=bytearray(bs[sid]);struct.pack_into('<I',shape,0,len(names));c['strings'].append(new.encode());struct.pack_into('<I',shape,skin_offset,skin2);bs.append(bytes(shape));c['ids'].append(c['ids'][sid])
 bs[parent]=bs[parent][:at]+struct.pack('<I',count+1)+struct.pack('<'+'I'*(count+1),*children,shape2)+bs[parent][end:]
 encoded=pack_container(c);newtri=duplicate_tri_shape(tri,source,new)
 before=parse_nif(data);after=parse_nif(encoded);orig=next(s for s in before if s['name']==source);rest=next(s for s in after if s['name']==source);foot=next(s for s in after if s['name']==new)
 require(rest['status']==foot['status']=='complete','roundtrip-shape-failed')
 require(orig['points']==rest['points']==foot['points'],'changed-points')
 require(orig['local']==rest['local']==foot['local'] and orig['skin']==rest['skin']==foot['skin'],'changed-transforms')
 combined=[None]*len(original_faces)
 for ids,s in [(body,rest),(foot_faces,foot)]:
  for i,t in zip(ids,s['triangles']):combined[i]=t
 require(combined==orig['triangles'],'not-a-face-partition')
 # Known block changes only; all other source blocks exactly retained.
 old=unpack_container(data);unchanged=[i for i in range(len(old['blocks'])) if i not in (part,parent)]
 require(all(old['blocks'][i]==c['blocks'][i] for i in unchanged),'unrelated-block-modification')
 return encoded,newtri,dict(sourceSha256=hash_(data),preparedSha256=hash_(encoded),sourceTriSha256=hash_(tri),preparedTriSha256=hash_(newtri),sourceShape=source,footShape=new,verticesPerShape=len(orig['points']),originalTriangles=len(original_faces),footTriangles=len(foot_faces),bodyTriangles=len(body),originalFaceIndexesForFoot=foot_faces,unchangedOriginalBlocks=len(unchanged),allVertexBytesPreserved=True,allOriginalMorphRecordsPreserved=True,fullVisibleUnionEqualsOriginal=True,runtimeVisibilityImplemented=False,preparedBodyTri=tri_resource)

