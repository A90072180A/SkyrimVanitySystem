"""Versioned, shoe-sharded runtime library for VHA >= 0.18 (not user pairs).

The published index is the commit point. Existing .user.json is never modified.
Binary files use explicit little-endian fields; no pickle or native struct dumps.
JSON export receipts retain source report hashes, warnings and group decisions.
"""
from __future__ import annotations
import hashlib
import math
import os
from pathlib import Path
import struct
import tempfile
import uuid

from .candidate_store import read_candidates, stream_json
from .formats import Resources, bounded_read, file_hash
from .loadorder import verify_sources
from vha_fileio import absolute_path, ensure_directory, optional_bytes, unlink_missing_ok

INDEX_MAGIC = b'VHAIDX1\0'
SHARD_MAGIC = b'VHASHD1\0'
MAX_SHOES = 4096
MAX_MEMBERS = 200000
MAX_SHARD_MEMBERS = 20000
MAX_INDEX_BYTES = 8*1024*1024
MAX_SHARD_BYTES = 16*1024*1024
RESIDUAL_WARNING = 1
SHARED_APPROXIMATION = 2
MANUAL_VALUE = 4
ALLOWED_STATUS = {'within-mathematical-limits', 'review-residual', 'manual-adjusted'}


def ascii_lower(value: str) -> str:
    return value.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz'))


def model_key(value: str) -> str:
    value=ascii_lower(value.replace('/', '\\'))
    if not value or value.startswith('\\') or any(ord(c)<32 for c in value):raise ValueError('unsafe resource path')
    parts=[p for p in value.split('\\') if p]
    if parts and parts[0]=='meshes':parts.pop(0)
    if not parts or any(p in ('.','..') or ':' in p for p in parts):raise ValueError('unsafe resource path')
    return '\\'.join(parts)


def identity(armor, addon, model):
    import height_editor
    if not height_editor.ID.fullmatch(armor) or not height_editor.ID.fullmatch(addon):raise ValueError('invalid stable armor/addon')
    result=ascii_lower(armor),ascii_lower(addon),model_key(model)
    if not result[2].endswith('.nif') or any(c in armor+addon for c in '/\\:') or any(ord(c)<32 for c in armor+addon):raise ValueError('invalid model identity')
    return result


def fnv(data: bytes) -> int:
    # Integrity/cache fingerprint, not a cryptographic signature. SHA256 remains
    # the publication filename and audit fingerprint in the exporter.
    value=14695981039346656037
    for byte in data:value=((value ^ byte)*1099511628211)&0xffffffffffffffff
    return value


class Writer:
    def __init__(self,magic):self.data=bytearray(magic)
    def u(self,v):self.data.extend(struct.pack('<I',v))
    def q(self,v):self.data.extend(struct.pack('<Q',v))
    def d(self,v):
        if not math.isfinite(v):raise ValueError('nonfinite library value')
        self.data.extend(struct.pack('<d',v))
    def s(self,v):
        b=v.encode('utf-8')
        if not b or len(b)>2048 or b'\0' in b:raise ValueError('invalid library string')
        self.u(len(b));self.data.extend(b)
    def id(self,v):
        for s in v:self.s(s)


class Reader:
    def __init__(self,data,magic):
        if not data.startswith(magic):raise ValueError('invalid library magic')
        self.data=data;self.pos=len(magic)
    def take(self,n):
        if n<0 or self.pos+n>len(self.data):raise ValueError('truncated library')
        result=self.data[self.pos:self.pos+n];self.pos+=n;return result
    def u(self,limit=0xffffffff):
        n=struct.unpack('<I',self.take(4))[0]
        if n>limit:raise ValueError('library count limit')
        return n
    def q(self):return struct.unpack('<Q',self.take(8))[0]
    def d(self):
        v=struct.unpack('<d',self.take(8))[0]
        if not math.isfinite(v):raise ValueError('nonfinite library value')
        return v
    def s(self):
        n=self.u(2048)
        if not n:raise ValueError('empty library string')
        v=self.take(n).decode('utf-8')
        if '\0' in v:raise ValueError('NUL library string')
        return v
    def id(self):return identity(self.s(),self.s(),self.s())
    def end(self):
        if self.pos!=len(self.data):raise ValueError('trailing library bytes')


def encode_shard(shard):
    w=Writer(SHARD_MAGIC);w.id(shard['shoe']);w.d(shard['weight'])
    assets=shard['assets'];groups=shard['groups'];members=shard['members']
    if len(assets)>65535 or len(groups)>MAX_SHARD_MEMBERS or len(members)>MAX_SHARD_MEMBERS:raise ValueError('shoe library limit')
    w.u(len(assets))
    for a in assets:w.s(a['resource']);w.q(a['fingerprint'])
    w.u(len(groups))
    for n,h in groups:w.d(n);w.d(h)
    w.u(len(members))
    for m in members:
        w.id(m['stocking']);w.u(m['group']);w.u(m['flags'])
        w.d(m['originalNoHeel']);w.d(m['originalHeel']);w.d(m['residual'])
        w.u(len(m['sources']))
        for i in m['sources']:w.u(i)
    data=bytes(w.data)
    if len(data)>MAX_SHARD_BYTES:raise ValueError('shoe shard byte limit')
    decode_shard(data) # same full validation for locally constructed data
    return data


def decode_shard(data):
    import height_editor
    if len(data)>MAX_SHARD_BYTES:raise ValueError('shoe shard byte limit')
    r=Reader(data,SHARD_MAGIC);shoe=r.id();weight=r.d()
    if not 0<=weight<=100:raise ValueError('invalid library weight')
    assets=[]
    for _ in range(r.u(65535)):
        path=model_key(r.s())
        if not path.endswith(('.nif','.tri')):raise ValueError('invalid library asset')
        assets.append({'resource':path,'fingerprint':r.q()})
    groups=[]
    for _ in range(r.u(MAX_SHARD_MEMBERS)):
        n,h=r.d(),r.d();height_editor.controls(n,h,10);groups.append((n,h))
    members=[];seen=set()
    for _ in range(r.u(MAX_SHARD_MEMBERS)):
        stock=r.id();g=r.u();flags=r.u(7);n,h,err=r.d(),r.d(),r.d()
        height_editor.controls(n,h,10)
        if err<0 or g>=len(groups) or stock in seen:raise ValueError('invalid/duplicate member')
        seen.add(stock);sources=[r.u() for _ in range(r.u(64))]
        if not sources or len(set(sources))!=len(sources) or any(i>=len(assets) for i in sources):raise ValueError('invalid member sources')
        if not {shoe[2],stock[2]} <= {assets[i]['resource'] for i in sources}:raise ValueError('missing model source binding')
        m={'stocking':stock,'group':g,'flags':flags,'originalNoHeel':n,'originalHeel':h,'residual':err,'sources':sources}
        members.append(m)
    r.end()
    return {'shoe':shoe,'weight':weight,'assets':assets,'groups':groups,'members':members}


def encode_index(entries,generation,stockings=()):
    if len(entries)>MAX_SHOES or sum(e['count'] for e in entries)>MAX_MEMBERS:raise ValueError('library index capacity')
    w=Writer(INDEX_MAGIC);w.s(generation);w.u(len(stockings))
    for stock in sorted(stockings):w.id(stock)
    w.u(len(entries))
    for e in sorted(entries,key=lambda e:e['shoe']):
        w.id(e['shoe']);w.s(e['file']);w.q(e['fingerprint']);w.u(e['count'])
    data=bytes(w.data)
    if len(data)>MAX_INDEX_BYTES:raise ValueError('index byte limit')
    decode_index(data);return data


def decode_index(data):
    if len(data)>MAX_INDEX_BYTES:raise ValueError('index byte limit')
    r=Reader(data,INDEX_MAGIC);generation=r.s();stocks=[r.id() for _ in range(r.u(20000))]
    if len(set(stocks))!=len(stocks):raise ValueError('duplicate stocking identity')
    entries=[];seen=set();total=0
    for _ in range(r.u(MAX_SHOES)):
        shoe=r.id();name=r.s();fingerprint=r.q();count=r.u(MAX_SHARD_MEMBERS)
        if shoe in seen or len(name)!=68 or not name.endswith('.vhs') or any(c not in '0123456789abcdef' for c in name[:-4]):raise ValueError('unsafe/duplicate index entry')
        seen.add(shoe);total+=count
        if total>MAX_MEMBERS:raise ValueError('library member capacity')
        entries.append({'shoe':shoe,'file':name,'fingerprint':fingerprint,'count':count})
    r.end();return {'generation':generation,'stockings':stocks,'entries':entries}


def write_atomic(path,data):
    ensure_directory(path.parent);fd,tmp=tempfile.mkstemp(prefix=path.name+'.',suffix='.tmp',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:unlink_missing_ok(Path(tmp))


def eligible(row,allow_residual=True):
    # High residual is a warning, not permission to use missing/invalid geometry.
    return (row.get('status') in ALLOWED_STATUS and not row.get('saturated',False)
            and (allow_residual or row.get('status')!='review-residual'))


def export_library(report_path:Path,plugins_dir:Path,indexes,allow_residual=True,
                   user_file=None,*,expected_sha256=None,group_report=None,group_indexes=None,cancelled=lambda:False):
    """Publish selected fixed suggestions; no .user.json writes, no 2048 cap.

    Existing unrelated library members are kept. Only explicitly selected
    members are replaced. Hand-written pairs, ignored items and legacy shoe
    settings remain higher priority in the runtime and are excluded here.
    """
    import height_editor
    def checkpoint():
        if cancelled():raise ValueError("export cancelled before publication; active library unchanged")
    checkpoint()
    report=read_candidates(report_path);report_hash=report['_storage']['sha256']
    if expected_sha256 and report_hash!=expected_sha256:raise ValueError('candidate report changed; reopen results')
    if report.get('generatorVersion') not in ('0.17.0-offline6','0.18.0-preview','0.18.0'):raise ValueError('recalculate unsupported candidate version')
    selected=list(indexes)
    if not selected or len(set(selected))!=len(selected) or any(type(i)!=int or not 0<=i<len(report['entries']) for i in selected):raise ValueError('invalid library selection')
    editor=height_editor.Editor(plugins_dir,user_file)
    protected={(ascii_lower(r['stocking']),ascii_lower(r['footwear'])) for r in editor.user.get('pairs',[])}
    protected.update((ascii_lower(r['stocking']),ascii_lower(r['footwear'])) for r in editor.base.get('signedHeightOverrides',[]))
    ignored={ascii_lower(r['armor']) for r in editor.user.get('items',[]) if r['kind']=='ignore'}
    legacy={ascii_lower(k) for k in editor.base.get('heels',{})}
    blocked={ascii_lower(x) for x in report.get('recordValidation',{}).get('blockedRecordIDs',[])}
    reference=report.get('reference',{})
    if any(ascii_lower(reference.get(k,'')) in blocked for k in ('armor','addon')):raise ValueError('quarantined reference')
    verify_sources(report['inputSources'])
    resources=Resources(Path(report['data']),[Path(x) for x in report.get('archivePaths',[])])
    # Resolve to the already established winning physical user overlay directory.
    root=absolute_path(editor.path.parent)/'VanityUBEHeelAdapter'/'height-library'
    old_bytes=optional_bytes(root/'index.vhi',MAX_INDEX_BYTES)
    old=decode_index(old_bytes) if old_bytes is not None else {'entries':[]}
    current={tuple(e['shoe']):e for e in old['entries']}
    group_values={}
    if group_report is not None:
        if group_report.get('sourceCandidateFileSHA256')!=report_hash:raise ValueError('shared-value report is stale')
        from .shoe_groups import propose
        check=propose(report,group_report['tolerance'],cross_family=group_report['crossResponseFamilies'],
            indexes=sorted({i for g in group_report['groups'] for i in g['memberIndexes']}|set(group_report.get('excludedIndexes',[]))))
        if check['groups']!=group_report['groups']:raise ValueError('group report changed; regenerate it')
        requested=list(group_indexes or [])
        if not requested or len(set(requested))!=len(requested) or any(type(i)!=int or not 0<=i<len(check['groups']) for i in requested):raise ValueError('invalid group selection')
        members=set()
        for i in requested:
            g=check['groups'][i]
            for j in g['memberIndexes']:
                if j in members:raise ValueError('overlapping shared groups')
                members.add(j);group_values[j]=(g['NoHeel'],g['Heel'])
        if members!=set(selected):raise ValueError('group member selection mismatch')
    cache={};updated={};applied=[];preserved=[];skipped=[];warning_count=0
    staged_identity=set()
    def source(src):
        token=(src['resource'],src['sha256'],src['kind'],src['path'])
        if token in cache:return cache[token]
        payload,now=resources.read(src['resource'])
        if now['sha256']!=src['sha256'] or now['kind']!=src['kind'] or os.path.normcase(str(absolute_path(Path(now['path']))))!=os.path.normcase(str(absolute_path(Path(src['path'])))):
            raise ValueError('source/provider changed: '+src['resource'])
        if now['kind']!='loose':raise ValueError('bulk runtime reader currently requires loose NIF/TRI resources; archive-only member not exported')
        out={'resource':model_key(src['resource']),'fingerprint':fnv(payload)};cache[token]=out;return out
    for i in selected:
        checkpoint()
        row=report['entries'][i]
        if not eligible(row,allow_residual):skipped.append({'index':i,'reason':row.get('status','unsupported')});continue
        if any(ascii_lower(row.get(k,'')) in blocked for k in ('stocking','stockingAddon','footwear','footwearAddon')):raise ValueError('quarantined pair')
        shoe=identity(row['footwear'],row['footwearAddon'],row['footwearModel']);sock=identity(row['stocking'],row['stockingAddon'],row['stockingModel'])
        if (sock[0],shoe[0]) in protected or sock[0] in ignored or shoe[0] in ignored or shoe[0] in legacy:
            preserved.append(i);continue
        if (shoe,sock) in staged_identity:raise ValueError('duplicate exact library pair')
        staged_identity.add((shoe,sock))
        values=group_values.get(i,(row['NoHeel'],row['Heel']));height_editor.controls(*values,editor.maximum)
        err=row.get('normalizedResidual')
        if isinstance(err,bool) or not isinstance(err,(int,float)) or not math.isfinite(err) or err<0:raise ValueError('invalid residual')
        if not row.get('sources'):raise ValueError('member has no verified sources')
        if any(src.get('kind')!='loose' for src in row['sources']):
            skipped.append({'index':i,'reason':'archive-only-source-not-supported-by-bulk-reader'});continue
        sources=[source(s) for s in row['sources']]
        if shoe not in updated:
            if shoe in current:
                e=current[shoe];payload=bounded_read(root/e['file'],MAX_SHARD_BYTES)
                if fnv(payload)!=e['fingerprint']:raise ValueError('existing library shard changed')
                shard=decode_shard(payload)
                if shard['shoe']!=shoe or len(shard['members'])!=e['count']:raise ValueError('existing shard identity/count mismatch')
                if abs(shard['weight']-report['context']['weight'])>1e-6:raise ValueError('existing shoe library has a different source weight; use a separate MO2 profile/library')
            else:shard={'shoe':shoe,'weight':report['context']['weight'],'assets':[],'groups':[],'members':[]}
            updated[shoe]=shard
        shard=updated[shoe]
        source_ids=[]
        for src in sources:
            if src not in shard['assets']:shard['assets'].append(src)
            si=shard['assets'].index(src)
            if si not in source_ids:source_ids.append(si)
        values=tuple(values)
        if values not in shard['groups']:shard['groups'].append(values)
        group_id=shard['groups'].index(values)
        flags=(RESIDUAL_WARNING if row['status']=='review-residual' or err>report.get('maxResidual',.15) else 0)|(MANUAL_VALUE if row['status']=='manual-adjusted' else 0)
        if values!=(row['NoHeel'],row['Heel']):flags|=SHARED_APPROXIMATION
        warning_count+=bool(flags&RESIDUAL_WARNING)
        member={'stocking':sock,'group':group_id,'flags':flags,'originalNoHeel':row['NoHeel'],'originalHeel':row['Heel'],
                'residual':err,'sources':source_ids}
        shard['members']=[m for m in shard['members'] if m['stocking']!=sock]+[member]
        applied.append(i)
    if not applied:
        return {'published':False,'library':str(root),'appliedIndexes':[],'preservedExistingIndexes':preserved,'skipped':skipped,'warningCount':0}
    # All validation is complete before publishing; immutable shards first.
    generation=uuid.uuid4().hex;new_entries=dict(current)
    for shoe,shard in updated.items():
        checkpoint()
        payload=encode_shard(shard);name=hashlib.sha256(payload).hexdigest()+'.vhs'
        write_atomic(root/name,payload)
        new_entries[shoe]={'shoe':shoe,'file':name,'fingerprint':fnv(payload),'count':len(shard['members'])}
    all_stockings=set()
    for shoe,e in new_entries.items():
        checkpoint()
        payload=bounded_read(root/e['file'],MAX_SHARD_BYTES)
        if fnv(payload)!=e['fingerprint']:raise ValueError('existing shard changed before publication')
        sh=decode_shard(payload)
        if sh['shoe']!=shoe or len(sh['members'])!=e['count']:raise ValueError('shard identity mismatch before publication')
        all_stockings.update(m['stocking'] for m in sh['members'])
    encoded=encode_index(list(new_entries.values()),generation,all_stockings)
    # Cross-process publication lock and compare-and-swap avoid lost exports.
    ensure_directory(root)
    lock=root/'publish.lock'
    fd=os.open(lock,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    try:
        os.close(fd)
        if optional_bytes(root/'index.vhi',MAX_INDEX_BYTES)!=old_bytes:raise ValueError('library changed during export; retry with current library')
        if file_hash(report_path)!=report_hash:raise ValueError('candidate file changed during export')
        checkpoint()
        write_atomic(root/'index.vhi',encoded)
    finally:unlink_missing_ok(lock)
    receipt={'schema':1,'minimumRuntimeVersion':'0.18.0','published':True,'generation':generation,'library':str(root),
        'sourceReportSHA256':report_hash,'appliedIndexes':applied,'preservedExistingIndexes':preserved,'skipped':skipped,
        'warningCount':warning_count,'totalMembers':sum(e['count'] for e in new_entries.values()),'shoeShards':len(new_entries),
        'groupValuesApplied':group_report is not None,'oldShardsKept':True,
        'previousGeneration':old.get('generation'),
        'groupProposal':({k:group_report.get(k) for k in ('tolerance','crossResponseFamilies')} if group_report else None),
        'selectedGroups':group_indexes,
        'semantics':'Explicit fixed offline suggestions, not live OBody calibration. The published index, not this receipt, is authoritative. No .user.json changes.'}
    try:
        stream_json(root/('receipt-'+generation+'.json'),receipt)
        stream_json(report_path.parent/'offline-library-receipt.json',receipt)
    except (OSError,ValueError) as exc:receipt['receiptWriteWarning']=str(exc)
    return receipt
