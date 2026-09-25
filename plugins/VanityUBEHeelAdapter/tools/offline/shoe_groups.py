"""Shoe-centric shared-value proposals, never automatic runtime defaults.

Tolerance describes a coefficient distance, not centimetres or a guaranteed fit.
Exact candidate values remain in the lossless candidate report. This summary is
review-only; importing it as a native user configuration is intentionally invalid.
"""
from __future__ import annotations
from collections import defaultdict
import hashlib
import math
import statistics
import struct
from .candidate_store import canonical, MAX_PAIRS


def response_info(shape, noheel, heel):
    """Fingerprint the actual weighted baseline + topology + both morph bases."""
    digest = hashlib.sha256(b'vha-stocking-response-v1\0')
    for field in (shape['points'], noheel, heel):
        digest.update(struct.pack('<I', len(field)))
        for xyz in field: digest.update(struct.pack('<3d', *xyz))
    digest.update(struct.pack('<I', len(shape['triangles'])))
    for tri in shape['triangles']: digest.update(struct.pack('<3I', *tri))
    digest.update(canonical({k:shape.get(k) for k in ('local','skin','parentTransforms')}).encode())
    info = {'family': digest.hexdigest(), 'shape': shape['name'],
            'semantics': 'Weighted source baseline and real morph response, not live OBody.'}
    for name, values in (('NoHeel', noheel), ('Heel', heel)):
        lengths = [sum(x*x for x in v) for v in values if any(x for x in v)]
        info[name] = {'nonzeroVertices': len(lengths),
                      'rmsDelta': math.sqrt(sum(lengths)/len(lengths)) if lengths else 0.,
                      'maxDelta': math.sqrt(max(lengths)) if lengths else 0.}
    return info


def propose(report, tolerance=0., *, cross_family=False, indexes=None):
    """Deterministic bounded clusters; no chain-link merging or wildcard members."""
    if isinstance(tolerance,bool) or not isinstance(tolerance,(int,float)) or not math.isfinite(tolerance) or not 0<=tolerance<=.1:
        raise ValueError('分组容差必须在 0–0.1；0 表示仅合并完全相同的数值。')
    rows=report['entries']
    if len(rows)>MAX_PAIRS:raise ValueError('group pair limit')
    selected=list(range(len(rows))) if indexes is None else list(indexes)
    if len(set(selected))!=len(selected) or any(type(i) is not int or not 0<=i<len(rows) for i in selected):
        raise ValueError('invalid group selection')
    buckets=defaultdict(list);excluded=[]
    eligible={'within-mathematical-limits','review-residual','manual-adjusted'}
    for i in selected:
        row=rows[i];n,h=row.get('NoHeel'),row.get('Heel')
        if (row.get('status') not in eligible or any(isinstance(x,bool) or not isinstance(x,(float,int)) or not math.isfinite(x) for x in (n,h))
                or not 0<=n<=1 or not 0<=h<=report['heelMax'] or (n>0 and h>0)):
            excluded.append(i);continue
        direction='Heel' if h>0 else 'NoHeel' if n>0 else 'baseline'
        family=row.get('stockingResponse',{}).get('family') or 'unknown:'+row['stocking']+'::'+row['stockingAddon']
        key=(row['footwear'],row['footwearAddon'],row['footwearModel'],direction,row['status'],'' if cross_family else family)
        buckets[key].append((i, h if h>0 else n))

    def centre(members, cap):
        mid=statistics.median(v for _,v in members)
        if tolerance==0:return mid
        rounded=round(mid,2)
        return rounded if 0<=rounded<=cap and max(abs(v-rounded) for _,v in members)<=tolerance else mid

    groups=[]
    def emit(key,members):
        shoe,addon,model,direction,status,_=key
        value=centre(members,report["heelMax"] if direction=="Heel" else 1.);families=set();max_rms=0.;max_vertex=0.;sensitivity_known=True
        for i,v in members:
            response=rows[i].get('stockingResponse',{})
            families.add(response.get('family') or 'unknown:'+rows[i]['stocking']+'::'+rows[i]['stockingAddon'])
            basis=response.get(direction)
            if direction=='baseline':continue
            if not basis:sensitivity_known=False;continue
            max_rms=max(max_rms,abs(v-value)*basis['rmsDelta'])
            max_vertex=max(max_vertex,abs(v-value)*basis['maxDelta'])
        spread=max(abs(v-value) for _,v in members)
        groups.append({'groupIndex':len(groups),'footwear':shoe,'footwearAddon':addon,'footwearModel':model,
            'footwearName':rows[members[0][0]]['footwearName'],'direction':direction,
            'NoHeel':value if direction=='NoHeel' else 0.,'Heel':value if direction=='Heel' else 0.,
            'memberIndexes':[i for i,_ in members],'memberCount':len(members),
            'minOriginal':min(v for _,v in members),'maxOriginal':max(v for _,v in members),
            'maxCoefficientChange':spread,'lossless':spread==0.,'sourceStatus':status,
            'responseFamilyCount':len(families),'sameMeasuredResponse':len(families)==1 and not next(iter(families)).startswith('unknown:'),
            'estimatedMaxMemberRMSChange':max_rms if sensitivity_known else None,
            'estimatedMaxVertexChange':max_vertex if sensitivity_known else None,
            'residualAtProposedValue':None,'requiresExplicitReview':True,
            'automaticApplicationAllowed':False})
    for key,items in sorted(buckets.items()):
        cluster=[];cap=report["heelMax"] if key[3]=="Heel" else 1.
        for item in sorted(items,key=lambda x:(x[1],x[0])):
            trial=cluster+[item]
            if cluster and max(abs(v-centre(trial,cap)) for _,v in trial)>tolerance:
                emit(key,cluster);cluster=[item]
            else:cluster=trial
        if cluster:emit(key,cluster)
    return {'schema':1,'kind':'vha-shoe-group-review','generatorVersion':report['generatorVersion'],
        'sourceCandidateFileSHA256':report.get('_storage',{}).get('sha256'),
        'context':report['context'],'heelMax':report['heelMax'],'NoHeelMaximum':1.,
        'tolerance':tolerance,'crossResponseFamilies':cross_family,'groups':groups,'excludedIndexes':excluded,
        'runtimeReadable':False,'automaticApplicationAllowed':False,
        'semantics':'Review-only. memberIndexes refer to the exact candidate file hash; no unseen stockings join. '
                    'Native 0.17 reads explicit user pairs, NOT this file. Raw values and fit status are retained '
                    'in offline-candidates.json. Cross-family numeric similarity is not shared slider semantics.'}
