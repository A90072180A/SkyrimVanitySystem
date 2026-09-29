#!/usr/bin/env python3
"""Build a private, atomic, exact-source CPB surface bundle. Requires matching DLL.

Input model bytes and the reviewed local delta are never published by this tool.
The original compact offline previews are NOT copied into the installable bundle.
"""
from __future__ import annotations
import argparse, hashlib, io, json, os, shutil, struct, tempfile, zipfile
from pathlib import Path
import numpy as np
from offline.formats import Reader, parse_nif, parse_tri, FormatError
from offline.mesh_partition_prep import split
from prepare_cpb_occlusion_patch import selected_faces, EXPECTED

MAIN='SB_bodystocking'; FOOT='VHA_CPB_CoveredFoot'; FIT='VHA_GlassFootFit'
CPB='!ube/caenarvon/cosplay/'; TRI=CPB+'vha/cpb_sb_surface_v1.tri'
AGATA='!ube/[spaz490]/witchy agata heels/agata'
GLASS='!ube/blacksmith/heelspack/flamingoshoes/flamingoshoes'
DELTA_SHA='a7661033d529540e6e30d05da5044d95d8186e99914c517e953cded1ffabcfe7'
SHOES={
 'agata':{'prefix':AGATA,'nif0':'2b25f5279f58cbe4d334ccfa14c9c63bd838a2c4cb0978f58c8446dba65fe074','nif1':'2b25f5279f58cbe4d334ccfa14c9c63bd838a2c4cb0978f58c8446dba65fe074','tri':'2b905e30520e1fa03217ad9aef9fa8bb60dda3e9b468f89ad27685b40858e496'},
 'glass':{'prefix':GLASS,'nif0':'d7c649777df869660dbe01cd880cfa41fc8e136e468fb7b6e5ee55561d8dafee','nif1':'90125ac3b7359c2e70e398b2eb0a51facf65bf9030a8e1998566ed570f99a208','tri':'4693b452ef04c817da3a0b9b7200fc3bbaf2047355764245a55a58751fb05d01'}}

def require(ok, reason):
 if not ok: raise FormatError(reason)
def sha(b): return hashlib.sha256(b).hexdigest()
def fnv(b):
 h=0xcbf29ce484222325
 for x in b: h=((h^x)*0x100000001b3)&0xffffffffffffffff
 return f'{h:016x}'
def canonical(s): return s.replace('\\','/').lower()
def read_member(z, suffix):
 names=[n for n in z.namelist() if canonical(n).endswith(canonical(suffix))]
 require(len(names)==1, 'missing-or-ambiguous-archive-member: '+suffix)
 info=z.getinfo(names[0]); require(info.file_size<=128*1024*1024, 'archive-member-too-large')
 return z.read(info)
def named(s):
 b=s.encode('utf8'); require(0<len(b)<=255, 'invalid-TRI-name'); return bytes([len(b)])+b

def add_fit(raw, delta):
 """Append one packed position morph to BOTH siblings; retain original bytes."""
 require(delta.shape==(23340,3) and np.isfinite(delta).all(), 'invalid-fit-array')
 require(float(np.linalg.norm(delta,axis=1).max())<=.301, 'fit-displacement-bound')
 ids=np.flatnonzero(np.linalg.norm(delta,axis=1)>1e-8)
 require(len(ids)==3228, 'unexpected-fit-domain')
 multiplier=np.float32(np.max(np.abs(delta))/32767.)
 packed=np.rint(delta[ids]/float(multiplier)).astype(np.int64)
 require(np.max(np.abs(packed))<=32767, 'fit-quantization-range')
 record=named(FIT)+struct.pack('<fH',multiplier,len(ids))
 record+=b''.join(struct.pack('<Hhhh',int(i),*map(int,v)) for i,v in zip(ids,packed))
 r=Reader(raw); require(r.take(4)==b'PIRT','unsupported-TRI'); count=r.u16(); out=b'PIRT'+struct.pack('<H',count); found=set()
 for _ in range(count):
  start=r.pos; name=r.name(); at=r.pos; nm=r.u16(); entries=r.pos
  for _ in range(nm):
   morph=r.name(); require(morph!=FIT, 'fit-already-present'); r.f32(); n=r.u16(); r.take(n*8)
  if name in (MAIN,FOOT):
   require(nm<65535, 'morph-count-limit'); out+=raw[start:at]+struct.pack('<H',nm+1)+raw[entries:r.pos]+record; found.add(name)
  else: out+=raw[start:r.pos]
 require(found=={MAIN,FOOT}, 'both-prepared-shapes-required'); out+=raw[r.pos:]
 old,_=parse_tri(raw); new,_=parse_tri(out)
 for shape, morphs in old.items():
  for name, values in morphs.items(): require(new[shape][name]==values, 'native-morph-changed')
 require(new[MAIN][FIT]==new[FOOT][FIT], 'fit-seam-mismatch')
 decoded=np.zeros_like(delta)
 for i,v in new[MAIN][FIT].items(): decoded[i]=v
 error=float(np.max(np.abs(decoded-delta)))
 require(error<=float(multiplier)*.501+1e-8, 'fit-quantization-error')
 return out, {'changedVertices':len(ids),'maxComponentError':error,'packingMultiplier':float(multiplier),'nativeMorphsPerShape':len(old[MAIN]),'fitIdenticalOnBothShapes':True}

def blocks_for(cpb_shapes, cpb_morphs, shoe_shapes, shoe_morphs, heel):
 # Nonzero sliders affecting the audited foot/ankle or any shoe geometry block fit.
 points=np.asarray(cpb_shapes[0]['points']); h=cpb_morphs[MAIN]['Heel']
 posed=points.copy()
 for i,v in h.items(): posed[i]+=heel*np.asarray(v)
 ids=set(map(int,np.flatnonzero(posed[:,2]<14.)))
 names={name for name,changes in cpb_morphs[MAIN].items() if any(i in ids and any(abs(x)>1e-8 for x in v) for i,v in changes.items())}
 for shape in shoe_shapes:
  for name,changes in shoe_morphs.get(shape['name'],{}).items():
   if any(any(abs(x)>1e-8 for x in v) for v in changes.values()): names.add(name)
 return sorted(names-{'Heel','NoHeel'})

def assemble(opaque_zip:Path, glass_zip:Path, prototype_zip:Path, output:Path):
 output=output.resolve(); require(not output.exists(), 'output-must-be-a-new-directory')
 with zipfile.ZipFile(opaque_zip) as az, zipfile.ZipFile(glass_zip) as gz, zipfile.ZipFile(prototype_zip) as pz:
  originals={name.lower():read_member(az,CPB+name) for name in EXPECTED}
  for name,expected in EXPECTED.items():
   b=originals[name.lower()]; require(sha(b)==expected,'CPB-source-hash-mismatch: '+name)
   require(read_member(gz,CPB+name)==b,'diagnostic-CPB-sources-differ')
  raw_delta=read_member(pz,'OFFLINE_ONLY/glass_fit/delta.npy')
  require(sha(raw_delta)==DELTA_SHA,'unreviewed-prototype-delta')
  delta=np.load(io.BytesIO(raw_delta),allow_pickle=False)
  src0=originals['cpb_sb_0.nif'];src1=originals['cpb_sb_1.nif'];srct=originals['cpb_sb.tri']
  faces=selected_faces(src0,src1,srct);require(len(faces)==5656,'prepared-mask-changed')
  n0,t0,a0=split(src0,srct,faces,source=MAIN,new=FOOT,tri_resource=TRI)
  n1,t1,a1=split(src1,srct,faces,source=MAIN,new=FOOT,tri_resource=TRI)
  require(t0==t1,'endpoint-TRI-mismatch');tri,fit_audit=add_fit(t0,delta)
  assets=[(CPB+'cpb_sb_0.nif',src0,n0),(CPB+'cpb_sb_1.nif',src1,n1),(CPB+'cpb_sb.tri',srct,tri)]
  aliases=[{'before':{'resource':old.replace('/','\\'),'fingerprint':fnv(a)},'after':{'resource':(TRI if old.endswith('.tri') else old).replace('/','\\'),'fingerprint':fnv(b)}} for old,a,b in assets]
  profile={'schema':1,'algorithm':'cpb-surface-v1-full-domain','generation':sha(tri),'sourceWeight':0.0,'aliases':aliases,'shoes':{}}
  cpb_shapes=parse_nif(src0); cpb_morphs,_=parse_tri(srct)
  for key,z in [('agata',az),('glass',gz)]:
   spec=SHOES[key]; checked=[];shoe={}
   for kind,suffix in [('nif0','_0.nif'),('nif1','_1.nif'),('tri','.tri')]:
    resource=spec['prefix']+suffix; b=read_member(z,resource);require(sha(b)==spec[kind],key+'-source-hash-mismatch: '+kind)
    checked.append({'resource':resource.replace('/','\\'),'fingerprint':fnv(b)});shoe[kind]=b
   sm,_=parse_tri(shoe['tri'])
   profile['shoes'][key]={'assets':checked,'blockedMorphs':blocks_for(cpb_shapes,cpb_morphs,parse_nif(shoe['nif0']),sm,.47 if key=='agata' else 1.1)}
  outputs={'Meshes/'+CPB+'cpb_sb_0.nif':n0,'Meshes/'+CPB+'cpb_sb_1.nif':n1,'Meshes/'+TRI:tri}
  profile_bytes=(json.dumps(profile,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode()
  outputs['SKSE/Plugins/VanityUBEHeelAdapter/prepared-cpb.json']=profile_bytes
  report={'schema':1,'algorithm':profile['algorithm'],'profileFingerprint':fnv(profile_bytes),'sourceHashes':EXPECTED,'prototypeDeltaSha256':DELTA_SHA,'fullDomainSplit':{'low':a0,'high':a1},'fit':fit_audit,'outputs':{p:{'sha256':sha(b),'fnv1a64':fnv(b),'bytes':len(b)} for p,b in outputs.items()},'runtimeDLLIncluded':False,'gameplayValidated':False,'requiresAdapter':'0.19.1','heightLibraryRewritten':False}
  outputs['VHA_CPB_SURFACE_BUILD.json']=(json.dumps(report,ensure_ascii=False,indent=2)+'\n').encode()
 output.parent.mkdir(parents=True,exist_ok=True)
 temp=Path(tempfile.mkdtemp(prefix='.'+output.name+'-',dir=output.parent))
 try:
  for path,data in outputs.items():
   dest=temp/path;dest.parent.mkdir(parents=True,exist_ok=True)
   with dest.open('xb') as f: f.write(data);f.flush();os.fsync(f.fileno())
  require(not output.exists(),'output-created-concurrently');os.rename(temp,output)
 except BaseException:
  shutil.rmtree(temp,ignore_errors=True);raise
 return report

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('opaque','glass','prototype','output'):p.add_argument('--'+name,required=True,type=Path)
 args=p.parse_args();print(json.dumps(assemble(args.opaque,args.glass,args.prototype,args.output),ensure_ascii=False,indent=2))
if __name__=='__main__':main()
