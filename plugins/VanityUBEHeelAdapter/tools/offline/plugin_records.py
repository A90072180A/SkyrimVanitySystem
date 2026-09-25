"""Read-only ARMO/ARMA decoding with explicit, non-resolving quarantine.

A file FormID indexes that file's MAST list plus itself, not the runtime load
order. Never guess an origin for an out-of-range reference. Recover only when
the containing record has a valid identity and its binary framing is intact.
"""
from __future__ import annotations
import mmap
from collections import defaultdict, deque
from pathlib import Path
import struct
from .formats import FormatError, MAX_RESOURCE, subrecords, text, sha256, decompress_zlib
from vha_fileio import handle_stamp, file_stamp, readable_file

MAX_REFERENCE_ISSUES = 4096

class PluginRecordError(FormatError):
    def __init__(self, code, **details):
        self.details = {'code': code, **details}
        location = ' '.join(f'{k}={v!r}' for k, v in details.items())
        super().__init__(f'{code}: {location}')

class FormReferenceError(PluginRecordError):
    """Invalid local master index; not a request to auto-enable a plugin."""


def _issue(report, detail):
    if report is not None:
        issues = report.setdefault('issues', [])
        if len(issues) >= MAX_REFERENCE_ISSUES:
            raise PluginRecordError('reference-diagnostic-limit', limit=MAX_REFERENCE_ISSUES)
        issues.append(detail)


def read_plugin(path: Path, canonical: dict[str, str], *, with_source=False,
                quarantine_references=False, diagnostics=None):
    """Strict by default; scanner may quarantine invalid *field* references.

    Header IDs, binary framing, I/O and master dependencies remain fatal. A
    quarantined override is returned as a blocking row, never dropped.
    """
    context = {'plugin': path.name, 'path': str(path)}
    try:
        with path.open('rb') as stream:
            before = handle_stamp(stream)
            if not 24 <= before[0] <= 2 * 1024**3:
                raise FormatError('plugin-size-limit')
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                if data[:4] != b'TES4':
                    raise FormatError('missing-TES4')
                head_size, head_flags = struct.unpack_from('<II', data, 4)
                if head_size > MAX_RESOURCE or head_size + 24 > len(data):
                    raise FormatError('bad-TES4-size')
                masters = [text(v) for k, v in subrecords(bytes(data[24:24+head_size])) if k == 'MAST']
                if len(masters) > 254 or len(set(x.casefold() for x in masters)) != len(masters):
                    raise FormatError('invalid-master-list')
                origin = [canonical.get(x.casefold(), x) for x in masters] + [canonical.get(path.name.casefold(), path.name)]
                context.update(masterCount=len(masters), originCount=len(origin), pluginFlags=f'0x{head_flags:08X}')

                def form(raw, field, occurrence=None):
                    if raw == 0:
                        return ''
                    idx, local = raw >> 24, raw & 0xffffff
                    if idx >= len(origin):
                        raise FormReferenceError('form-master-index-out-of-range', **context,
                            field=field, occurrence=occurrence, rawFormID=f'0x{raw:08X}',
                            rawBytes=struct.pack('<I', raw).hex(), masterIndex=idx,
                            interpretation='file-local-master-index; no runtime remapping attempted')
                    return f'{origin[idx]}|{local:08X}'

                result = []
                def walk(start, end, depth=0):
                    if depth > 32:
                        raise FormatError('plugin-group-depth')
                    while start < end:
                        if end - start < 24:
                            raise FormatError('truncated-plugin-header')
                        sig = bytes(data[start:start+4])
                        size, flags = struct.unpack_from('<II', data, start+4)
                        if sig == b'GRUP':
                            if size < 24 or start+size > end:
                                raise FormatError('invalid-GRUP-size')
                            group_type = struct.unpack_from('<i', data, start+12)[0]
                            if group_type != 0 or bytes(data[start+8:start+12]) in (b'ARMO', b'ARMA'):
                                walk(start+24, start+size, depth+1)
                            start += size
                            continue
                        if size > MAX_RESOURCE or start+24+size > end:
                            raise FormatError('invalid-record-size')
                        if sig in (b'ARMO', b'ARMA'):
                            raw = struct.unpack_from('<I', data, start+12)[0]
                            context.update(recordType=sig.decode(), recordRawFormID=f'0x{raw:08X}',
                                recordOffset=start, recordOffsetHex=f'0x{start:X}',
                                recordFlags=f'0x{flags:08X}', formVersion=struct.unpack_from('<H', data, start+20)[0],
                                compressed=bool(flags & 0x40000), editorID='')
                            # Identity must be valid before recovery is allowed.
                            context.pop('recordID', None)
                            identity = form(raw, 'record-header')
                            if not identity:
                                raise PluginRecordError('null-record-identity', **context)
                            context['recordID'] = identity
                            row = {'id': identity, 'type': sig.decode(), 'deleted': bool(flags & 0x20),
                                'name': identity, 'editorID': '', 'owner': path.name, 'slots': 0,
                                'race': '', 'sourcePlugin': path.name, 'quarantined': False}
                            # Deleted winners are tombstones: never inspect stale model references.
                            if row['deleted']:
                                result.append(row)
                                start += 24+size
                                continue
                            payload = bytes(data[start+24:start+24+size])
                            if flags & 0x40000:
                                if len(payload) < 4:
                                    raise FormatError('truncated-compressed-record')
                                payload = decompress_zlib(payload[4:], struct.unpack_from('<I', payload)[0])
                            fields = {}
                            for key, value in subrecords(payload):
                                fields.setdefault(key, []).append(value)
                            def first(key, default=b''):
                                return fields.get(key, [default])[0]
                            def ref(key):
                                value = first(key)
                                if not value:
                                    return ''
                                if len(value) != 4:
                                    raise PluginRecordError('invalid-form-reference', **context, field=key, byteCount=len(value))
                                return form(struct.unpack('<I', value)[0], key, 0)
                            def refs(key):
                                values = []
                                for occurrence, value in enumerate(fields.get(key, [])):
                                    if len(value) != 4:
                                        raise PluginRecordError('invalid-form-reference', **context, field=key,
                                                                occurrence=occurrence, byteCount=len(value))
                                    x = form(struct.unpack('<I', value)[0], key, occurrence)
                                    if x:
                                        values.append(x)
                                return values
                            slots = first('BOD2', first('BODT'))
                            if slots and len(slots) < 4:
                                raise FormatError('truncated-slot-mask')
                            edid = text(first('EDID'))
                            context['editorID'] = edid
                            name = edid
                            if not head_flags & 0x80 and first('FULL'):
                                name = text(first('FULL')) or name
                            row.update(name=name, editorID=edid,
                                       slots=struct.unpack_from('<I', slots)[0] if slots else 0)
                            try:
                                row['race'] = ref('RNAM')
                                if sig == b'ARMO':
                                    row.update(addons=refs('MODL'), template=ref('TNAM'))
                                else:
                                    d = first('DNAM')
                                    row.update(femaleModel=text(first('MOD3')), maleModel=text(first('MOD2')),
                                        femaleWeightSlider=bool(len(d) > 3 and d[3] & 2), races=refs('MODL'))
                            except FormReferenceError as exc:
                                if not quarantine_references:
                                    raise
                                detail = {**exc.details, 'action': 'quarantine-record-not-plugin'}
                                _issue(diagnostics, detail)
                                # Whole record is blocked, not merely the failing array element.
                                row.update(quarantined=True, parseIssue=detail, addons=[], template='',
                                    race='', races=[], femaleModel='', maleModel='', femaleWeightSlider=False)
                            result.append(row)
                        start += 24+size
                walk(24+head_size, len(data))
                digest = sha256(data)
                if before != handle_stamp(stream):
                    raise FormatError('plugin-changed-during-scan')
        if before != file_stamp(path):
            raise FormatError('plugin-replaced-during-scan')
    except PluginRecordError:
        raise
    except (FormatError, struct.error, UnicodeError) as exc:
        raise PluginRecordError(str(exc), **context) from exc
    if with_source:
        return masters, result, {'kind': 'plugin', 'path': str(path), 'sha256': digest}
    return masters, result


def load_records(data: Path, plugins: list[str], progress=lambda s: None, *,
                 quarantine_references=False, diagnostics=None):
    canonical = {x.casefold(): x for x in plugins}
    seen, records, sources = set(), {}, []
    if diagnostics is not None:
        diagnostics.update(state='reading-plugins', issues=[], processedPlugins=0, totalPlugins=len(plugins))
    for i, name in enumerate(plugins):
        progress(f'插件 {i+1}/{len(plugins)}：{name}')
        path = data/name
        if diagnostics is not None:
            diagnostics.update(currentPlugin=name, currentPluginIndex=i)
        try:
            masters, rows, source = read_plugin(path, canonical, with_source=True,
                quarantine_references=quarantine_references, diagnostics=diagnostics)
        except FileNotFoundError as exc:
            raise FileNotFoundError(
                f'无法直接打开启用插件：{path}\n请确认从 MO2 启动，Data/profile 属于同一实例。'
                '不会跳过启用插件；可运行 tools/vha_io_probe.py。') from exc
        for m in masters:
            if m.casefold() not in seen:
                if m.casefold() in canonical:
                    why, detail = 'late-active-master', '主文件已在列表中，但排在依赖它的插件后面。'
                elif readable_file(data/m):
                    why, detail = 'master-not-active', '主文件可以打开，但未被当前 profile 或 Skyrim.ccc 纳入。'
                else:
                    why, detail = 'master-not-visible', '当前进程无法在所选虚拟 Data 中打开主文件。'
                raise PluginRecordError(why, plugin=name, master=m, detail=detail)
        sources.append(source)
        for row in rows:
            key = row['id'].casefold()
            if key in records and records[key]['type'] != row['type']:
                raise PluginRecordError('form-type-conflict', plugin=name, recordID=row['id'])
            records[key] = row  # Includes deleted and quarantined winners.
        seen.add(name.casefold())
        if diagnostics is not None:
            diagnostics['processedPlugins'] = i+1
    blocked = {key for key, row in records.items() if row.get('quarantined')}
    for row in records.values():
        if row.get('quarantined'):
            row['blockedBy'] = [row['id']]
    # Propagate through reverse edges in linear time, including template chains.
    reverse = defaultdict(list)
    for key, row in records.items():
        if row['type'] == 'ARMO' and not row['deleted'] and not row.get('quarantined'):
            for link in [*row.get('addons', []), row.get('template', '')]:
                if link:
                    reverse[link.casefold()].append(key)
    pending = deque(blocked)
    while pending:
        for key in reverse[pending.popleft()]:
            if key not in blocked:
                blocked.add(key)
                pending.append(key)
    for key in blocked:
        row = records[key]
        if not row.get('quarantined'):
            row['blockedBy'] = [link for link in [*row.get('addons', []), row.get('template', '')]
                                if link.casefold() in blocked]
    if diagnostics is not None:
        diagnostics.update(state='parsed-with-quarantine' if blocked else 'parsed',
            issueCount=len(diagnostics['issues']),
            quarantinedWinnerCount=sum(bool(r.get('quarantined')) for r in records.values()),
            blockedRecordIDs=[records[k]['id'] for k in sorted(blocked)])
        for issue in diagnostics['issues']:
            winner = records.get(issue['recordID'].casefold())
            # Stored dictionaries are deliberately independent; compare stable provenance.
            issue['isWinningIssue'] = bool(winner and winner.get('quarantined')
                and winner.get('parseIssue', {}).get('plugin') == issue['plugin']
                and winner.get('parseIssue', {}).get('recordOffset') == issue['recordOffset'])
    return records, sources
