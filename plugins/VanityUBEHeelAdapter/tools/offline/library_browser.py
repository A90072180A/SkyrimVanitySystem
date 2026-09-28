"""Read the active 0.18 height library, not old receipts/orphan shard files.

No library/config/model writes. Optional reports and diagnostic model bundles
are published to explicit new destinations only. Reading is not a runtime fit.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from .bulk_library import (decode_index, decode_shard, fnv, identity,
    MAX_INDEX_BYTES, MAX_SHARD_BYTES, RESIDUAL_WARNING, SHARED_APPROXIMATION, MANUAL_VALUE)
from .formats import bounded_read
from .catalog_query import text_match
from .candidate_store import stream_json
from vha_fileio import absolute_path, ensure_directory, unlink_missing_ok

VERSION = '0.18.0-library1'


class LibraryBrowser:
    def __init__(self, root: Path):
        self.root = absolute_path(root)
        raw = bounded_read(self.root/'index.vhi', MAX_INDEX_BYTES)
        self.index = decode_index(raw)
        self.index_sha256 = hashlib.sha256(raw).hexdigest()
        self.entries = {tuple(e['shoe']): e for e in self.index['entries']}
        self.stockings = set(self.index['stockings'])
        self.shard_reads = 0
        self._cache = {}

    def check_current(self):
        raw = bounded_read(self.root/'index.vhi', MAX_INDEX_BYTES)
        if hashlib.sha256(raw).hexdigest() != self.index_sha256:
            raise ValueError('高度库已更新，请刷新索引后重选鞋子；不会混合新旧代际。')

    def shoes(self, query='', names=None):
        names = names or {}
        return [dict(e, name=names.get(k, '')) for k, e in self.entries.items()
                if text_match('\n'.join((*k, names.get(k, ''))), query)]

    def read_shoe(self, shoe):
        self.check_current()
        key = identity(*shoe)
        if key not in self.entries:
            raise ValueError('这双鞋的 ARMO / ARMA / 模型组合不在当前高度库中。')
        entry = self.entries[key]
        if key not in self._cache:
            payload = bounded_read(self.root/entry['file'], MAX_SHARD_BYTES)
            self.shard_reads += 1
            if (fnv(payload) != entry['fingerprint'] or
                    hashlib.sha256(payload).hexdigest()+'.vhs' != entry['file']):
                raise ValueError('鞋文件内容指纹不匹配；未显示损坏或被替换的数据。')
            shard = decode_shard(payload)
            if shard['shoe'] != key or len(shard['members']) != entry['count']:
                raise ValueError('鞋文件身份或成员数量与当前索引不一致。')
            if any(m['stocking'] not in self.stockings for m in shard['members']):
                raise ValueError('鞋文件含有不在当前索引中的丝袜成员。')
            self._cache = {key: shard}  # one selected shoe; not all shoe files
        self.check_current()
        # Independent copies: the caller cannot mutate the cached binary view.
        shard = json.loads(json.dumps(self._cache[key], allow_nan=False))
        members = []
        for i, m in enumerate(shard['members']):
            n, h = shard['groups'][m['group']]
            flags = m['flags']
            members.append(dict(m, memberIndex=i, NoHeel=n, Heel=h,
                residualWarning=bool(flags & RESIDUAL_WARNING),
                sharedApproximation=bool(flags & SHARED_APPROXIMATION),
                manualValue=bool(flags & MANUAL_VALUE),
                residualAtAppliedValue=None if flags & (SHARED_APPROXIMATION | MANUAL_VALUE) else m['residual']))
        buckets = {}
        for m in members:buckets.setdefault(m['group'], []).append(m['memberIndex'])
        groups = [dict(groupIndex=i, NoHeel=v[0], Heel=v[1], memberIndexes=buckets.get(i, []))
            for i, v in enumerate(shard['groups'])]
        return dict(schema=1, generatorVersion=VERSION, library=str(self.root),
            generation=self.index['generation'], indexSHA256=self.index_sha256,
            shoe=list(key), shard=entry['file'], weight=shard['weight'],
            assets=shard['assets'], groups=[g for g in groups if g['memberIndexes']], members=members,
            semantics='Read-only saved controls, not effective in-game values. Manual/ignore rules may override. '
                      'Groups are stored control-table entries, not original user-named groups. '
                      'Assets have not been revalidated against the installed game merely by viewing.')

    def export_report(self, shoe, output: Path):
        report = self.read_shoe(shoe)
        output = self._new_destination(output, '.json')
        stream_json(output, report)
        return report

    def _new_destination(self, output, suffix):
        output = absolute_path(output)
        if output.suffix.lower() != suffix or output.is_relative_to(self.root):
            raise ValueError('请选择高度库目录之外的新 '+suffix+' 文件；不覆盖高度库或配置。')
        # Open, not stat(), for MO2 virtual-file existence.
        try:
            with output.open('rb'): pass
        except FileNotFoundError:
            return output
        raise ValueError('输出文件已存在，请选择新文件名。')

    def bundle_models(self, shoe, indexes, data: Path, output: Path):
        """Copy ONLY source-bound NIF/TRI of <=16 selected pairs, locally.

        No game assets enter the repository. No editing, automatic hiding,
        textures/BodySlide sources, body presets or full mod directory collection.
        """
        report = self.read_shoe(shoe)
        ids = list(indexes)
        if not ids or len(ids)>16 or len(set(ids))!=len(ids) or any(type(i)!=int or not 0<=i<len(report['members']) for i in ids):
            raise ValueError('诊断打包请选择 1–16 个明确的丝袜成员。')
        wanted = sorted({s for i in ids for s in report['members'][i]['sources']})
        data = absolute_path(data)
        output = self._new_destination(output, '.zip')
        ensure_directory(output.parent)
        fd, tmp = tempfile.mkstemp(prefix=output.stem+'.', suffix='.tmp', dir=output.parent)
        os.close(fd)
        manifest = dict(schema=1, generatorVersion=VERSION, purpose='Private clipping diagnosis; not an installable patch',
            generation=report['generation'], shoe=report['shoe'], weight=report['weight'],
            members=[report['members'][i] for i in ids], assets=[], totalBytes=0,
            missingScope='Does not include textures or BodySlide OSP/ShapeData. No visibility/fit fix has been applied.')
        try:
            with ZipFile(tmp, 'w', compression=ZIP_DEFLATED) as z:
                for j in wanted:
                    src = report['assets'][j]
                    resource = src['resource']  # validated safe relative NIF/TRI by decode_shard
                    payload = bounded_read(data/'Meshes'/Path(resource.replace('\\','/')), 64*1024*1024)
                    if fnv(payload)!=src['fingerprint']:
                        raise ValueError('模型与已保存高度库的来源不一致，未打包：'+resource)
                    manifest['totalBytes'] += len(payload)
                    if manifest['totalBytes']>256*1024*1024:raise ValueError('诊断包超过 256 MiB 未压缩上限，请减少成员。')
                    name = 'Meshes/'+resource.replace('\\','/')
                    z.writestr(name, payload)
                    manifest['assets'].append(dict(resource=resource, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest(),
                        libraryFingerprint=f'{src["fingerprint"]:016x}'))
                z.writestr('DIAGNOSTIC-MANIFEST.json', json.dumps(manifest, ensure_ascii=False, allow_nan=False, indent=2))
            self.check_current()
            with open(tmp, 'rb+') as f:os.fsync(f.fileno())
            # Refuse accidental replacement even if another program created it.
            self._new_destination(output, '.zip')
            os.replace(tmp, output)
        finally:unlink_missing_ok(Path(tmp))
        return manifest
