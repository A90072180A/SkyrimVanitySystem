"""Bounded, read-only TES5 records, SSE NIF, packed TRI and BSA readers.

NIF/TRI subset mirrors this project's independently tested portable C++ cores.
Unsupported geometry is reported, never interpreted as flat feet. No game assets
are modified. Plugin IDs remain origin-plugin/local IDs, not runtime load indices.
"""
from __future__ import annotations
import hashlib
import math
import mmap
from vha_fileio import handle_stamp, file_stamp, readable_file
from pathlib import Path, PureWindowsPath
import struct
import zlib

MAX_RESOURCE = 64 * 1024 * 1024

class FormatError(ValueError):
    pass

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def file_hash(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def bounded_read(path: Path, limit: int = MAX_RESOURCE) -> bytes:
    with path.open('rb') as f:
        before = handle_stamp(f)
        data = f.read(limit + 1)
        if before != handle_stamp(f):
            raise FormatError('resource-changed-during-read')
    if len(data) > limit:
        raise FormatError('resource-size-limit')
    return data

def text(data: bytes) -> str:
    data = data.rstrip(b'\0')
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return data.decode('cp1252', errors='replace')

def relative_model(name: str) -> str:
    value = name.replace('/', '\\')
    p = PureWindowsPath(value)
    if p.is_absolute() or p.drive or '..' in p.parts or '\x00' in value or ':' in value:
        raise FormatError('unsafe-resource-path')
    parts = [v for v in p.parts if v not in ('.', '')]
    if parts and parts[0].casefold() == 'meshes':
        parts = parts[1:]
    if not parts:
        raise FormatError('empty-resource-path')
    return '\\'.join(parts)

class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0
    @property
    def left(self):
        return len(self.data) - self.pos
    def take(self, n):
        if n < 0 or n > self.left:
            raise FormatError('truncated-record')
        v = self.data[self.pos:self.pos+n]
        self.pos += n
        return v
    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))
    def u8(self): return self.unpack('B')[0]
    def u16(self): return self.unpack('H')[0]
    def u32(self): return self.unpack('I')[0]
    def u64(self): return self.unpack('Q')[0]
    def f32(self):
        v = self.unpack('f')[0]
        if not math.isfinite(v): raise FormatError('nonfinite-number')
        return v
    def boolean(self):
        n = self.u8()
        if n > 1: raise FormatError('invalid-bool')
        return bool(n)
    def count(self, maximum):
        n = self.u32()
        if n > maximum: raise FormatError('count-limit')
        return n
    def name(self):
        b = self.take(self.u8())
        if not b or b'\0' in b: raise FormatError('invalid-name')
        return text(b)
    def done(self):
        if self.left: raise FormatError('trailing-data')

def decompress_zlib(data: bytes, expected: int) -> bytes:
    if not 0 <= expected <= MAX_RESOURCE:
        raise FormatError('decompression-size-limit')
    dec = zlib.decompressobj()
    out = dec.decompress(data, expected + 1)
    if len(out) != expected or not dec.eof or dec.unused_data or dec.unconsumed_tail:
        raise FormatError('invalid-zlib-size-or-stream')
    return out

def subrecords(data: bytes):
    r = Reader(data)
    extra = None
    while r.left:
        sig, size = r.take(4), r.u16()
        if sig == b'XXXX':
            if size != 4 or extra is not None: raise FormatError('invalid-XXXX')
            extra = r.u32()
            continue
        if extra is not None:
            size, extra = extra, None
        yield sig.decode('ascii', 'strict'), r.take(size)
    if extra is not None: raise FormatError('dangling-XXXX')

def active_plugins(data: Path, profile: Path) -> list[str]:
    """MO2 SSE profile: starred plugins only, with official implicit masters."""
    p = profile / 'plugins.txt'
    lines = bounded_read(p, 1024 * 1024).decode('utf-8-sig').splitlines()
    enabled = [line.strip()[1:] for line in lines if line.strip().startswith('*')]
    if not enabled:
        raise FormatError('plugins.txt has no * enabled entries; select the actual MO2 SSE profile')
    implicit = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']
    enabled = [n for n in implicit if readable_file(data/n)] + enabled
    names = {}
    for n in enabled:
        if PureWindowsPath(n).name != n or '\\' in n or '/' in n or Path(n).suffix.lower() not in ('.esp', '.esm', '.esl'):
            raise FormatError('unsafe-plugin-name')
        names.setdefault(n.casefold(), n)
    order_path = profile/'loadorder.txt'
    ordered = []
    if readable_file(order_path):
        for line in bounded_read(order_path, 1024*1024).decode('utf-8-sig').splitlines():
            n = line.strip().lstrip('*')
            if n.casefold() in names and n.casefold() not in {x.casefold() for x in ordered}:
                ordered.append(names[n.casefold()])
    # Official masters may be omitted by profile exporters.
    ordered = [n for n in implicit if n.casefold() in names and n not in ordered] + ordered
    ordered += [n for n in names.values() if n.casefold() not in {x.casefold() for x in ordered}]
    return ordered

def read_plugin(path: Path, canonical: dict[str, str], *, with_source=False):
    """Yield winning-record candidates; traverse only top-level ARMO/ARMA groups."""
    with path.open('rb') as stream:
        before = handle_stamp(stream)
        if not 24 <= before[0] <= 2 * 1024**3:
            raise FormatError('plugin-size-limit')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            if data[:4] != b'TES4': raise FormatError('missing-TES4')
            head_size, head_flags = struct.unpack_from('<II', data, 4)
            if head_size > MAX_RESOURCE or head_size+24 > len(data): raise FormatError('bad-TES4-size')
            masters = [text(v) for k,v in subrecords(bytes(data[24:24+head_size])) if k=='MAST']
            if len(masters)>254 or len(set(x.casefold() for x in masters)) != len(masters):
                raise FormatError('invalid-master-list')
            origin = [canonical.get(x.casefold(), x) for x in masters] + [canonical.get(path.name.casefold(), path.name)]
            def form(raw):
                if raw == 0: return ''
                idx, local = raw >> 24, raw & 0xffffff
                if idx >= len(origin): raise FormatError('form-master-index-out-of-range')
                return f'{origin[idx]}|{local:08X}'
            result = []
            def walk(start, end, depth=0):
                if depth>32: raise FormatError('plugin-group-depth')
                while start<end:
                    if end-start<24: raise FormatError('truncated-plugin-header')
                    sig = bytes(data[start:start+4]); size, flags = struct.unpack_from('<II',data,start+4)
                    if sig==b'GRUP':
                        if size<24 or start+size>end: raise FormatError('invalid-GRUP-size')
                        group_type=struct.unpack_from('<i',data,start+12)[0]
                        if group_type != 0 or bytes(data[start+8:start+12]) in (b'ARMO',b'ARMA'):
                            walk(start+24,start+size,depth+1)
                        start += size; continue
                    if size>MAX_RESOURCE or start+24+size>end: raise FormatError('invalid-record-size')
                    if sig in (b'ARMO',b'ARMA'):
                        raw=struct.unpack_from('<I',data,start+12)[0]
                        payload=bytes(data[start+24:start+24+size])
                        if flags & 0x40000:
                            if len(payload)<4: raise FormatError('truncated-compressed-record')
                            payload=decompress_zlib(payload[4:],struct.unpack_from('<I',payload)[0])
                        fields={}
                        for k,v in subrecords(payload): fields.setdefault(k,[]).append(v)
                        def first(k,default=b''): return fields.get(k,[default])[0]
                        def ref(k):
                            v=first(k)
                            if not v:return ''
                            if len(v)!=4:raise FormatError('invalid-form-reference')
                            return form(struct.unpack('<I',v)[0])
                        def refs(k):
                            values=[]
                            for v in fields.get(k,[]):
                                if len(v)!=4:raise FormatError('invalid-form-reference')
                                x=form(struct.unpack('<I',v)[0])
                                if x:values.append(x)
                            return values
                        slots=first('BOD2',first('BODT'))
                        if slots and len(slots)<4:raise FormatError('truncated-slot-mask')
                        name=text(first('EDID'))
                        full=first('FULL')
                        if not head_flags&0x80 and full: name=text(full) or name
                        row={'id':form(raw),'type':sig.decode(),'deleted':bool(flags&0x20),'name':name,
                             'editorID':text(first('EDID')),'owner':path.name,'slots':struct.unpack_from('<I',slots)[0] if slots else 0,
                             'race':ref('RNAM'),'sourcePlugin':path.name}
                        if sig==b'ARMO':row.update(addons=refs('MODL'),template=ref('TNAM'))
                        else:
                            d=first('DNAM')
                            row.update(femaleModel=text(first('MOD3')),maleModel=text(first('MOD2')),
                                       femaleWeightSlider=bool(len(d)>3 and d[3]&2),races=refs('MODL'))
                        result.append(row)
                    start+=24+size
            walk(24+head_size,len(data))
            # Hash exactly the mapped file that was parsed, not a second open.
            digest = sha256(data)
            if before != handle_stamp(stream):
                raise FormatError('plugin-changed-during-scan')
    if before != file_stamp(path):
        raise FormatError('plugin-replaced-during-scan')
    if with_source:
        return masters,result,{'kind':'plugin','path':str(path),'sha256':digest}
    return masters,result

def load_records(data: Path, plugins: list[str], progress=lambda s:None):
    canonical={x.casefold():x for x in plugins}; seen=set(); records={}; sources=[]
    for i,name in enumerate(plugins):
        progress(f'插件 {i+1}/{len(plugins)}：{name}')
        path=data/name
        try:
            masters,rows,source=read_plugin(path,canonical,with_source=True)
        except FileNotFoundError as exc:
            raise FileNotFoundError(
                f"无法直接打开启用插件：{path}\n"
                "请确认使用 MO2 的运行按钮启动 Python，Data 与当前 profile 属于同一游戏实例，"
                "并检查 MO2 右侧 Data 中是否能看到该插件。不会跳过启用的主文件。\n"
                "可运行 tools/vha_io_probe.py 收集 open/fstat/stat 的对照结果。"
            ) from exc
        for m in masters:
            if m.casefold() not in seen:
                raise FormatError(f'{name}: missing/late active master {m}; check plugins/loadorder')
        sources.append(source)
        for row in rows:
            key=row['id'].casefold()
            if key in records and records[key]['type']!=row['type']:raise FormatError('form-type-conflict')
            records[key]=row
        seen.add(name.casefold())
    return records,sources

def parse_tri(data: bytes, wanted: set[str]|None=None):
    if len(data)>MAX_RESOURCE:raise FormatError('TRI-size-limit')
    r=Reader(data)
    if r.take(4)!=b'PIRT':raise FormatError('unsupported-TRI-format')
    output={}; inventory={}
    def section(dim,keep):
        shapes=r.u16()
        if shapes>4096:raise FormatError('TRI-shape-limit')
        seen_shapes=set()
        for _ in range(shapes):
            shape=r.name()
            if shape in seen_shapes:raise FormatError('duplicate-TRI-shape')
            seen_shapes.add(shape);count=r.u16()
            if count>4096:raise FormatError('TRI-morph-limit')
            seen=set()
            if keep: output[shape]={};inventory[shape]=[]
            for _ in range(count):
                name=r.name()
                if name in seen:raise FormatError('duplicate-TRI-morph')
                seen.add(name);scale=r.f32();n=r.u16()
                if scale<0:raise FormatError('negative-TRI-scale')
                block=r.take(n*(2+2*dim))
                if keep:inventory[shape].append(name)
                if not keep or (wanted is not None and name not in wanted):continue
                offsets={}
                for values in struct.iter_unpack('<H'+'h'*dim,block):
                    idx=values[0]
                    if idx in offsets:raise FormatError('duplicate-TRI-vertex')
                    delta=tuple(struct.unpack('<f',struct.pack('<f',x*scale))[0] for x in values[1:])
                    if not all(math.isfinite(x) for x in delta):raise FormatError('nonfinite-TRI-offset')
                    offsets[idx]=delta
                output[shape][name]=offsets
    section(3,True)
    if r.left:section(2,False)
    r.done()
    return output,inventory

def transform(r: Reader, translation_first=True):
    t=tuple(r.f32() for _ in range(3)) if translation_first else None
    rot=tuple(r.f32() for _ in range(9))
    if t is None:t=tuple(r.f32() for _ in range(3))
    return {'rotation':rot,'translation':t,'scale':r.f32()}

def parse_partition(data):
    r=Reader(data)
    if r.u32()!=1:raise FormatError('unsupported-multiple-partitions')
    size,stride,desc=r.u32(),r.u32(),r.u64()
    if not 16<=stride<=256 or size%stride or (desc&15)*4!=stride:raise FormatError('invalid-stride')
    nv=size//stride
    if not 0<nv<=65535 or not desc>>44&1:raise FormatError('unsupported-position-stream')
    offsets=[((desc>>(4*i+2))&0x3c) for i in range(1,10) if (desc>>44)&(1<<i)]
    if min(offsets,default=stride)!=16 or any(x<16 or x>=stride for x in offsets):raise FormatError('unsupported-vertex-layout')
    raw=r.take(size)
    count,nt,nb,ns,nw=r.unpack('5H')
    if count!=nv or not nt or nb>256 or ns!=0 or nw!=4:raise FormatError('unsupported-partition-domain')
    r.take(nb*2)
    if r.boolean() and r.unpack('H'*nv)!=tuple(range(nv)):raise FormatError('nonidentity-vertex-map')
    if r.boolean():r.take(nv*nw*4)
    if not r.boolean():raise FormatError('missing-faces')
    tris=[tuple(t) for t in struct.iter_unpack('<3H',r.take(nt*6))]
    if any(max(t)>=nv for t in tris):raise FormatError('triangle-out-of-bounds')
    if not any(len(set(t))==3 for t in tris):raise FormatError('all-indices-degenerate')
    if r.boolean():r.take(nv*nw)
    r.take(2)
    if r.u64()!=desc:raise FormatError('partition-descriptor-mismatch')
    if tris!=list(struct.iter_unpack('<3H',r.take(nt*6))):raise FormatError('triangle-domain-mismatch')
    r.done()
    points=[struct.unpack_from('<3f',raw,i*stride) for i in range(nv)]
    if not all(math.isfinite(x) for p in points for x in p):raise FormatError('nonfinite-NIF-position')
    return points,tris

def parse_nif(data: bytes):
    if len(data)>MAX_RESOURCE:raise FormatError('NIF-size-limit')
    r=Reader(data);header=bytearray()
    while len(header)<128:
        header+=r.take(1)
        if header[-1]==10:break
    if header!=b'Gamebryo File Format, Version 20.2.0.7\n':raise FormatError('unsupported-NIF-header')
    if (r.u32(),r.u8(),r.u32())!=(0x14020007,1,12):raise FormatError('unsupported-NIF-version')
    n=r.count(4096)
    if not n or r.u32()!=100:raise FormatError('unsupported-NIF-stream')
    for _ in range(3):r.take(r.u8())
    types_n=r.u16()
    if not 0<types_n<=4096:raise FormatError('NIF-type-limit')
    types=[text(r.take(r.count(65536))) for _ in range(types_n)]
    ids=r.unpack('H'*n);sizes=r.unpack('I'*n);ns=r.count(65536);r.u32()
    strings=[text(r.take(r.count(65536))) for _ in range(ns)]
    for _ in range(r.count(4096)):r.u32()
    if any(i>=types_n for i in ids):raise FormatError('NIF-type-index')
    blocks=[(types[i],r.take(size)) for i,size in zip(ids,sizes)]
    roots=[r.u32() for _ in range(r.count(4096))]
    if any(i>=n for i in roots):raise FormatError('NIF-root-index')
    r.done()
    def string(i):
        if i==0xffffffff:return ''
        if i>=ns:raise FormatError('NIF-string-index')
        return strings[i]
    def obj(br):
        name=string(br.u32());extra=[br.u32() for _ in range(br.count(4096))]
        br.u32();br.u32();local=transform(br);br.u32()
        paths=[]
        for idx in extra:
            if idx>=n:raise FormatError('NIF-extra-index')
            if blocks[idx][0]=='NiStringExtraData':
                er=Reader(blocks[idx][1]);k,v=string(er.u32()),string(er.u32());er.done()
                if k=='BODYTRI' and v:paths.append(relative_model(v))
        return name,local,paths
    parents={};objects={};readers={}
    for i,(kind,raw) in enumerate(blocks):
        if kind not in ('NiNode','BSTriShape'):continue
        br=Reader(raw);objects[i]=obj(br);readers[i]=br
        if kind=='NiNode':
            for _ in range(br.count(4096)):
                child=br.u32()
                if child==0xffffffff:continue
                if child>=n or child in parents:raise FormatError('NIF-ambiguous-parent')
                parents[child]=i
    shapes=[]
    for i,(kind,raw) in enumerate(blocks):
        if kind!='BSTriShape':continue
        br=readers[i];name,local,paths=objects[i]
        current=i;chain=[];parent_transforms=[]
        while current in parents:
            if current in chain:raise FormatError('NIF-parent-cycle')
            chain.append(current);current=parents[current]
            parent_obj=objects.get(current)
            if parent_obj is None:raise FormatError('unsupported-parent-type')
            parent_transforms.append(parent_obj[1])
            if not paths:paths=parent_obj[2]
        shape={'name':name,'local':local,'parentTransforms':parent_transforms,'bodyTris':paths,'status':'not-read'}
        try:
            for _ in range(4):br.f32()
            skinid=br.u32();br.u32();br.u32();br.u64();br.u16();br.u16();size=br.u32()
            if size:raise FormatError('unsupported-inline-NIF-geometry')
            if br.u32()!=0:raise FormatError('unsupported-NIF-particles')
            br.done()
            if skinid>=n or blocks[skinid][0] not in ('NiSkinInstance','BSDismemberSkinInstance'):raise FormatError('unsupported-NIF-skin')
            sr=Reader(blocks[skinid][1]);dataid,partid=sr.u32(),sr.u32()
            if dataid>=n or partid>=n or blocks[dataid][0]!='NiSkinData' or blocks[partid][0]!='NiSkinPartition':raise FormatError('invalid-NIF-skin-links')
            shape['skin']=transform(Reader(blocks[dataid][1]),False)
            shape['points'],shape['triangles']=parse_partition(blocks[partid][1]);shape['status']='complete'
        except FormatError as e:shape['status']=str(e)
        shapes.append(shape)
    return shapes

def lz4_block(data: bytes, expected: int) -> bytes:
    if not 0<=expected<=MAX_RESOURCE:raise FormatError('LZ4-size-limit')
    r=Reader(data);out=bytearray()
    def length(n):
        if n==15:
            while True:
                b=r.u8();n+=b
                if b!=255:break
        return n
    while r.left:
        token=r.u8();lit=length(token>>4)
        if len(out)+lit>expected:raise FormatError('LZ4-overflow')
        out.extend(r.take(lit))
        if not r.left:break
        offset=r.u16()
        if not 0<offset<=len(out):raise FormatError('LZ4-offset')
        count=length(token&15)+4
        if len(out)+count>expected:raise FormatError('LZ4-overflow')
        for _ in range(count):out.append(out[-offset])
    if len(out)!=expected:raise FormatError('LZ4-size-mismatch')
    return bytes(out)

class Archive:
    """SSE BSA 104/105 named archives; caller determines active load priority."""
    def __init__(self,path:Path):
        self.path=path;self.entries={}
        with path.open('rb') as f:
            self.stamp=handle_stamp(f)
            header=f.read(36)
            if len(header)!=36 or header[:4]!=b'BSA\0':raise FormatError('unsupported-archive')
            version,offset,flags,folders,files,dirlen,namelen,_=struct.unpack('<8I',header[4:])
            if version not in (104,105) or flags&3!=3 or folders>100000 or files>1000000 or namelen>MAX_RESOURCE:
                raise FormatError('unsupported-BSA-header')
            self.version,self.flags=version,flags
            f.seek(offset);records=[]
            for _ in range(folders):
                raw=f.read(24 if version==105 else 16)
                if len(raw)!=(24 if version==105 else 16):raise FormatError('truncated-BSA-folder')
                count=struct.unpack_from('<I',raw,8)[0];records.append(count)
            if sum(records)!=files:raise FormatError('BSA-count-mismatch')
            pending=[]
            for count in records:
                b=f.read(1)
                if not b:raise FormatError('truncated-BSA-name')
                folder=f.read(b[0])
                if len(folder)!=b[0] or not folder.endswith(b'\0'):raise FormatError('invalid-BSA-folder-name')
                folder=text(folder)
                for _ in range(count):
                    raw=f.read(16)
                    if len(raw)!=16:raise FormatError('truncated-BSA-file')
                    _,size,pos=struct.unpack('<QII',raw)
                    length=size&0x3fffffff
                    if pos+length>self.stamp[0]:raise FormatError('BSA-file-bounds')
                    pending.append((folder,size,pos))
            names=f.read(namelen)
            parts=names.split(b'\0')
            if len(names)!=namelen or len(parts)!=files+1 or parts[-1]!=b'':raise FormatError('invalid-BSA-filenames')
            for (folder,size,pos),name in zip(pending,parts):
                key=(folder+'\\'+text(name)).replace('/','\\').casefold().strip('\\')
                if key in self.entries:raise FormatError('duplicate-BSA-resource')
                self.entries[key]=(size,pos)
            if self.stamp!=handle_stamp(f):raise FormatError('BSA-changed-during-scan')
    def read(self,key):
        size,pos=self.entries[key];length=size&0x3fffffff
        if length>MAX_RESOURCE:raise FormatError('BSA-resource-size-limit')
        with self.path.open('rb') as f:
            if self.stamp!=handle_stamp(f):raise FormatError('BSA-changed-during-scan')
            f.seek(pos);raw=f.read(length)
            if self.stamp!=handle_stamp(f):raise FormatError('BSA-changed-during-scan')
        if len(raw)!=length:raise FormatError('truncated-BSA-resource')
        r=Reader(raw)
        if self.flags&0x100:r.take(r.u8())
        payload=r.take(r.left)
        compressed=bool(self.flags&4)^bool(size&0x40000000)
        if compressed:
            if len(payload)<4:raise FormatError('truncated-BSA-compressed-header')
            expected=struct.unpack_from('<I',payload)[0]
            payload=lz4_block(payload[4:],expected) if self.version==105 else decompress_zlib(payload[4:],expected)
        return payload

class Resources:
    def __init__(self,data:Path,archives:list[Path]=()):
        self.data=data;self.archives=[Archive(p) for p in archives];self.cache={}
    def read(self,model:str):
        key='meshes\\'+relative_model(model)
        loose=self.data.joinpath(*key.split('\\'))
        try:
            payload=bounded_read(loose)
        except (FileNotFoundError, NotADirectoryError):
            pass
        else:
            return payload,{'kind':'loose','resource':key,'path':str(loose),'sha256':sha256(payload)}
        for archive in reversed(self.archives):
            if key.casefold() in archive.entries:
                payload=archive.read(key.casefold())
                return payload,{'kind':'bsa','resource':key,'path':str(archive.path),'sha256':sha256(payload)}
        raise FileNotFoundError('resource not found in loose Data or selected archives: '+key)
