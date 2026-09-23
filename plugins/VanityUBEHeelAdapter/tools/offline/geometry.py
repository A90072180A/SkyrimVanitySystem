"""Height-only measurement; Python counterpart of SurfacePosture/HeightPlanCore.

No normal-gap tightening, no global scaling, no guessed millimetre-to-slider map.
The reported coefficients are model-space recommendations, not visual proof.
"""
from __future__ import annotations
import math
from .formats import FormatError

def add(a,b):return (a[0]+b[0],a[1]+b[1],a[2]+b[2])
def sub(a,b):return (a[0]-b[0],a[1]-b[1],a[2]-b[2])
def mul(a,t):return (a[0]*t,a[1]*t,a[2]*t)
def dot(a,b):return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
def norm2(a):return dot(a,a)

def project(p,a,b,c):
    best=(math.inf,None,None)
    for u,v,i,j in ((a,b,0,1),(b,c,1,2),(c,a,2,0)):
        e=sub(v,u);n=norm2(e);t=max(0.,min(1.,dot(sub(p,u),e)/n)) if n else 0.
        q=add(u,mul(e,t));d=norm2(sub(p,q));w=[0.,0.,0.];w[i]=1-t;w[j]=t
        if d<best[0]:best=(d,q,tuple(w))
    u,v,w=sub(b,a),sub(c,a),sub(p,a)
    uu,vv,uv,uw,vw=dot(u,u),dot(v,v),dot(u,v),dot(u,w),dot(v,w)
    determinant=uu*vv-uv*uv
    if determinant>1e-14*max(uu*vv,1e-30):
        s,t=(uw*vv-vw*uv)/determinant,(vw*uu-uw*uv)/determinant
        if s>=0 and t>=0 and s+t<=1:
            q=add(a,add(mul(u,s),mul(v,t)));d=norm2(sub(p,q))
            if d<=best[0]:best=(d,q,(1-s-t,s,t))
    return best

def validate_mesh(p,t):
    if not 0<len(p)<=65535 or not 0<len(t)<=65535:raise FormatError('mesh-count-limit')
    if any(len(x)!=3 or not all(math.isfinite(v) for v in x) for x in p):raise FormatError('invalid-position')
    if any(len(x)!=3 or min(x)<0 or max(x)>=len(p) for x in t):raise FormatError('invalid-triangle')

class Surface:
    def __init__(self,points,triangles):
        validate_mesh(points,triangles);self.points=points;self.triangles=triangles
        centres=[tuple(sum(points[i][k] for i in tri)/3 for k in range(3)) for tri in triangles]
        bounds=[(tuple(min(points[i][k] for i in tri) for k in range(3)),
                 tuple(max(points[i][k] for i in tri) for k in range(3))) for tri in triangles]
        def build(ids):
            lo=tuple(min(bounds[i][0][k] for i in ids) for k in range(3))
            hi=tuple(max(bounds[i][1][k] for i in ids) for k in range(3))
            if len(ids)<=8:return (lo,hi,None,None,ids)
            axis=max(range(3),key=lambda k:hi[k]-lo[k]);ids.sort(key=lambda i:centres[i][axis]);mid=len(ids)//2
            return lo,hi,build(ids[:mid]),build(ids[mid:]),None
        self.root=build(list(range(len(triangles))))
    def nearest(self,p):
        best=(math.inf,None,None)
        def gap(node):
            lo,hi=node[:2]
            return sum(max(lo[k]-p[k],p[k]-hi[k],0.)**2 for k in range(3))
        def search(node):
            nonlocal best
            if gap(node)>best[0]:return
            if node[4] is not None:
                for idx in node[4]:
                    t=self.triangles[idx];hit=project(p,*(self.points[i] for i in t))
                    if hit[0]<best[0]:best=(hit[0],t,hit[2])
                return
            a,b=node[2:4]
            if gap(b)<gap(a):a,b=b,a
            search(a);search(b)
        search(self.root);return best

def align(anchor,at,target,tt):
    """Exact ordered topology or uniquely removed complete small components."""
    validate_mesh(anchor,at);validate_mesh(target,tt)
    if len(anchor)==len(target) and at==tt:
        return target,at,{'method':'exact-topology','removedVertices':0}
    missing=len(anchor)-len(target)
    if missing<=0 or missing>len(anchor)//10 or len(tt)>len(at):raise FormatError('unsupported-topology')
    parent=list(range(len(anchor)))
    def root(i):
        while parent[i]!=i:parent[i]=parent[parent[i]];i=parent[i]
        return i
    for t in at:
        r=root(t[0]);parent[root(t[1])]=r;parent[root(t[2])]=r
    groups={}
    for i in range(len(anchor)):groups.setdefault(root(i),[]).append(i)
    small=[v for k,v in sorted(groups.items()) if len(v)<=256 and len(v)<=missing]
    if not small or len(small)>16:raise FormatError('unsupported-topology')
    suffix=[0]*(len(small)+1)
    for i in range(len(small)-1,-1,-1):suffix[i]=suffix[i+1]+len(small[i])
    attempts=0;matches=[]
    def verify(removed):
        nonlocal attempts
        attempts+=1
        if attempts>128:return
        mapping={i:j for j,i in enumerate(i for i in range(len(anchor)) if i not in removed)}
        common=[t for t in at if not any(i in removed for i in t)]
        if len(common)!=len(tt):return
        if all(tuple(mapping[i] for i in a)==b for a,b in zip(common,tt)):
            aligned=[target[mapping[i]] if i in mapping else anchor[i] for i in range(len(anchor))]
            matches.append((aligned,common))
    def visit(i,left,removed):
        if len(matches)>1 or attempts>128 or left>suffix[i]:return
        if left==0:verify(removed);return
        if i==len(small):return
        visit(i+1,left,removed)
        if len(small[i])<=left:visit(i+1,left-len(small[i]),removed|set(small[i]))
    visit(0,missing,set())
    if attempts>128:raise FormatError('subset-budget-exceeded')
    if len(matches)!=1:raise FormatError('ambiguous-or-unsupported-topology')
    return *matches[0],{'method':'exact-ordered-component-subset','removedVertices':missing}

def build_map(anchor,triangles,flat_stocking,noheel,heel,max_gap=1.5,min_count=256):
    if not len(flat_stocking)==len(noheel)==len(heel):raise FormatError('donor-array-size-mismatch')
    surface=Surface(anchor,triangles);entries=[];eligible=0;gaps=[]
    for i,p in enumerate(flat_stocking):
        if max(norm2(noheel[i]),norm2(heel[i]))<.0001:continue
        eligible+=1;d,t,w=surface.nearest(p)
        if d>max_gap**2:continue
        entries.append((i,t,w));gaps.append(d)
    coverage=len(entries)/eligible if eligible else 0.
    return {'entries':entries,'eligible':eligible,'coverage':coverage,
            'rmsGap':math.sqrt(sum(gaps)/len(gaps)) if gaps else None,
            'status':'insufficient-correspondences' if len(entries)<min_count else 'insufficient-coverage' if coverage<.85 else 'mapped'}

def fit(mapping,anchor,target,noheel,heel,heel_max=2.,reference_noheel=1.):
    if mapping['status']!='mapped':raise FormatError(mapping['status'])
    if not math.isfinite(heel_max) or not 1<=heel_max<=10:raise FormatError('invalid-Heel-limit')
    desired=[];n=[];h=[]
    for idx,tri,w in mapping['entries']:
        d=(0.,0.,0.)
        for i,weight in zip(tri,w):d=add(d,mul(sub(target[i],anchor[i]),weight))
        desired.append(add(d,mul(noheel[idx],reference_noheel)));n.append(noheel[idx]);h.append(heel[idx])
    def branch(basis,maximum):
        denominator=sum(norm2(x) for x in basis)
        if denominator<=1e-20:return None
        raw=sum(dot(a,b) for a,b in zip(desired,basis))/denominator
        value=max(0.,min(maximum,raw))
        error=sum(norm2(sub(d,mul(b,value))) for d,b in zip(desired,basis))
        return {'raw':raw,'value':value,'error':error,'saturated':raw>maximum+1e-8}
    bn,bh=branch(n,1.),branch(h,heel_max)
    if bn is None and bh is None:raise FormatError('singular-height-basis')
    use_n=bn is not None and (bh is None or bn['error']<=bh['error'])
    chosen=bn if use_n else bh
    den=sum(norm2(v) for v in n) or sum(norm2(v) for v in h)
    return {'NoHeel':chosen['value'] if use_n else 0.,'Heel':0. if use_n else chosen['value'],
            'rawNoHeel':None if bn is None else bn['raw'],'rawHeel':None if bh is None else bh['raw'],
            'normalizedResidual':math.sqrt(chosen['error']/den),'rmsResidual':math.sqrt(chosen['error']/len(desired)),
            'saturated':chosen['saturated'],'correspondences':len(desired),'coverage':mapping['coverage'],'rmsGap':mapping['rmsGap']}
