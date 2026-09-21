#!/usr/bin/env python3
"""Audit exported foot snapshots. Read-only inputs; never writes a game profile.

Python 3.10+, standard library only. ZIPs are read without extracting their paths.
Usage: python audit_foot_snapshots.py snapshots.zip --output snapshot-audit.json
"""
from __future__ import annotations
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import struct
import tempfile
import zipfile

MAX_FILE = 24 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024
MASK = (1 << 64) - 1

def hash_u32(values):
    h = 14695981039346656037
    for value in values:
        for b in struct.pack('<I', value):
            h = ((h ^ b) * 1099511628211) & MASK
    return f'{h:016x}'

def sources(path: Path):
    total = 0
    if path.is_file() and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for item in sorted(archive.infolist(), key=lambda x:x.filename):
                if not item.filename.endswith('.json') or Path(item.filename).name.startswith('foot-') is False:
                    continue
                total += item.file_size
                if item.file_size > MAX_FILE or total > MAX_TOTAL:
                    raise ValueError('input-size limit exceeded')
                yield item.filename, archive.read(item)
    elif path.is_dir():
        for item in sorted(path.rglob('foot-*.json')):
            size = item.stat().st_size; total += size
            if size > MAX_FILE or total > MAX_TOTAL:
                raise ValueError('input-size limit exceeded')
            yield str(item.relative_to(path)), item.read_bytes()
    else:
        raise ValueError('input must be a snapshot directory or ZIP')

def check(name, data):
    d = json.loads(data.decode('utf-8-sig'))
    positions, triangles = d.get('positions',[]), d.get('triangles',[])
    n, m = len(positions), len(triangles)
    issues = []
    if d.get('extraction',{}).get('status') != 'complete': issues.append('extraction-not-complete')
    if not 0 < n <= 65535 or not 0 < m <= 65535: issues.append('invalid-counts')
    try:
        if any(len(v)!=3 or any(isinstance(x,bool) or not isinstance(x,(float,int)) or not math.isfinite(x) for x in v) for v in positions):
            issues.append('nonfinite-or-invalid-position')
        if any(len(t)!=3 or any(type(i) is not int or not 0<=i<n for i in t) for t in triangles):
            issues.append('invalid-triangle-index')
    except TypeError:
        issues.append('invalid-array-element')
    report = {'file':name,'inputSha256':hashlib.sha256(data).hexdigest(),
              'identity':d.get('identity',{}),'vertexCount':n,'triangleCount':m,
              'extractionStatus':d.get('extraction',{}).get('status'),
              'issues':issues,'posture':None,'status':'audited-not-calibrated'}
    if issues: return report,d
    th = hash_u32(itertools.chain((n,m*3),itertools.chain.from_iterable(triangles)))
    bits = (struct.unpack('<I',struct.pack('<f',float(x) if x else 0.0))[0] for v in positions for x in v)
    ph = hash_u32(itertools.chain((n,),bits))
    report.update(topologyFingerprint=th, positionFingerprint=ph,
                  declaredTopologyMatches=th==d.get('topologyFingerprint'),
                  declaredPositionsMatch=ph==d.get('positionFingerprint'),
                  allVerticesReferenced=len(set(itertools.chain.from_iterable(triangles)))==n,
                  repeatedIndexTriangles=sum(len(set(t))<3 for t in triangles),
                  bounds={'min':[min(v[k] for v in positions) for k in range(3)],
                          'max':[max(v[k] for v in positions) for k in range(3)]})
    for key,flag in [('topology-hash-mismatch','declaredTopologyMatches'),('position-hash-mismatch','declaredPositionsMatch')]:
        if not report[flag]: issues.append(key)
    e = d.get('extraction',{})
    if e.get('gpuAllocationBytes') != n*e.get('strideBytes',0): issues.append('buffer-size-mismatch')
    report['noHeelSources']=[{'resource':x.get('resource'),'status':x.get('status'),'offsets':len(x.get('offsets',[]))} for x in d.get('triMorphData',[])]
    return report,d

def context(d):
    identity = d.get('identity',{})
    return {k:identity.get(k) for k in ('actorWeight','race','sexIndex')},d.get('coordinateSpace'),d.get('localTransform'),d.get('rootParentToSkin')

def audit(path):
    records=[]; valid=[]
    for name,data in sources(path):
        try:
            record,d=check(name,data); records.append(record)
            if not record['issues']: valid.append((record,d))
        except (ValueError,TypeError,KeyError,OverflowError,struct.error) as error:
            records.append({'file':name,'issues':[str(error)],'posture':None})
    pairs=[]
    for (a,x),(b,y) in itertools.combinations(valid,2):
        same = x['triangles']==y['triangles'] and len(x['positions'])==len(y['positions'])
        item={'a':a['file'],'b':b['file'],'exactOrderedTopologyEqual':same,
              'contextEqual':context(x)==context(y),
              'actorMorphValuesEqual':x.get('actorMorphValues')==y.get('actorMorphValues')}
        if same:
            sq=[sum((float(u)-v)**2 for u,v in zip(p,q)) for p,q in zip(x['positions'],y['positions'])]
            item.update(positionArraysExactlyEqual=x['positions']==y['positions'],rmsVertexDistance=math.sqrt(sum(sq)/len(sq)))
        pairs.append(item)
    groups={}
    for r,d in valid: groups.setdefault(r['topologyFingerprint'],[]).append(r['file'])
    return {'schema':1,'status':'audit-only','input':path.name,'snapshotCount':len(records),'validCount':len(valid),
            'snapshots':records,'exactTopologyGroups':groups,'pairs':pairs,
            'automaticProfileWritten':False,
            'limitations':['A matching topology fingerprint is not certified semantic vertex correspondence.',
                           'Runtime morph declarations do not certify unmorphed or baked model state.',
                           'No reference NoHeel calibration is inferred from a shoe name or a relative fit.']}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args(); report=audit(args.input)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile('w',encoding='utf-8',dir=args.output.parent,delete=False) as f:
        json.dump(report,f,indent=2,ensure_ascii=False,allow_nan=False);f.write('\n'); temp=Path(f.name)
    temp.replace(args.output)
    print(f"{report['validCount']}/{report['snapshotCount']} valid snapshots; no game configuration changed")
if __name__=='__main__':main()
