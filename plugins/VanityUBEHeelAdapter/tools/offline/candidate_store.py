"""Lossless, bounded candidate storage. This is NOT a native user-config schema.

Repeated item metadata, source records/sets and identical analysis results are
interned. Float values are never rounded; per-pair status and provenance survive.
Only the final rename publishes a report. No pickle, archive extraction or paths
from a reference table are executed here.
"""
from __future__ import annotations
from collections import Counter
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from .formats import bounded_read
from vha_fileio import ensure_directory, unlink_missing_ok

FORMAT = 'vha-candidates-interned-v1'
LIMIT = 64 * 1024 * 1024
MAX_PAIRS = 20000
MAX_TABLE = 100000
MAX_EXPANDED = 512 * 1024 * 1024
STOCK = ('stocking', 'stockingAddon', 'stockingModel', 'stockingName')
SHOE = ('footwear', 'footwearAddon', 'footwearModel', 'footwearName')
SPECIAL = {'index', 'sources', 'stockingResponse', *STOCK, *SHOE}


def canonical(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True,
                      separators=(',', ':'))


def stream_json(path: Path, value, limit=LIMIT, *, compact=False):
    """Bound UTF-8 bytes while streaming; failed writes leave the old file intact."""
    if type(limit) is not int or limit < 1:
        raise ValueError('invalid output limit')
    ensure_directory(path.parent)
    fd, name = tempfile.mkstemp(prefix=path.name+'.', suffix='.tmp', dir=path.parent)
    count = 0
    digest = hashlib.sha256()
    encoder = json.JSONEncoder(ensure_ascii=False, allow_nan=False,
                              indent=None if compact else 2,
                              separators=(',', ':') if compact else None)
    try:
        with os.fdopen(fd, 'wb', buffering=256*1024) as stream:
            for chunk in encoder.iterencode(value):
                data = chunk.encode('utf-8'); count += len(data)
                if count + 1 > limit:
                    raise ValueError(f'output exceeds size limit: {path.name}; '
                                     f'at least {count+1:,} UTF-8 bytes, limit {limit:,}')
                stream.write(data); digest.update(data)
            stream.write(b'\n'); digest.update(b'\n'); count += 1
            stream.flush(); os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        unlink_missing_ok(Path(name))
    return {'bytes': count, 'sha256': digest.hexdigest(), 'path': str(path)}


class Table:
    def __init__(self):
        self.values = []; self.lookup = {}
    def add(self, value):
        token = canonical(value)
        if token not in self.lookup:
            if len(self.values) >= MAX_TABLE:
                raise ValueError('candidate dictionary limit')
            self.lookup[token] = len(self.values)
            self.values.append(value)
        return self.lookup[token]


def pack(report):
    rows = report.get('entries')
    if report.get('schema') != 1 or not isinstance(rows, list) or len(rows) > MAX_PAIRS:
        raise ValueError('invalid logical candidate report')
    stockings, shoes, sources, sets, results, responses = (Table() for _ in range(6))
    links = []
    for index, row in enumerate(rows):
        if type(row.get('index')) is not int or row['index'] != index:
            raise ValueError('candidate index/order mismatch')
        if any(not isinstance(row.get(k), str) for k in STOCK+SHOE):
            raise ValueError('invalid candidate item identity')
        source_set = None
        if 'sources' in row:
            if not isinstance(row['sources'], list) or len(row['sources']) > 256:
                raise ValueError('candidate source-set limit')
            source_set = sets.add([sources.add(s) for s in row['sources']])
        links.append([stockings.add({k: row[k] for k in STOCK}),
                      shoes.add({k: row[k] for k in SHOE}),
                      results.add({k: v for k, v in row.items() if k not in SPECIAL}),
                      source_set, responses.add(row["stockingResponse"]) if "stockingResponse" in row else None])
    metadata = {k: v for k, v in report.items() if k not in ('schema', 'entries', '_storage')}
    metadata['counts'] = dict(Counter(r.get('status', '') for r in rows))
    return {'schema': 2, 'format': FORMAT, 'generatorVersion': report.get('generatorVersion'),
            'metadata': metadata,
            'tables': {'stockings': stockings.values, 'footwear': shoes.values,
                       'sources': sources.values, 'sourceSets': sets.values, 'results': results.values, 'responses': responses.values},
            'entryColumns': ['stocking', 'footwear', 'result', 'sourceSet', 'response'], 'entries': links,
            'semantics': 'Lossless references, no rounding or shared runtime defaults. '
                         'Status, raw controls and per-pair evidence remain authoritative.'}


def unpack(value):
    if not isinstance(value, dict):
        raise ValueError('invalid candidate root')
    if value.get('schema') == 1:
        # Legacy logical reports are accepted by the decoder only. Application
        # still requires the current generator and source validation in engine.
        rows = value.get('entries')
        if not isinstance(rows, list) or len(rows) > MAX_PAIRS:
            raise ValueError('candidate row limit')
        if any(not isinstance(r, dict) or type(r.get('index')) is not int or r['index'] != i
               for i, r in enumerate(rows)):
            raise ValueError('candidate index/order mismatch')
        return value
    if value.get('schema') != 2 or value.get('format') != FORMAT:
        raise ValueError('unsupported candidate storage format')
    tables = value.get('tables'); metadata = value.get('metadata'); links = value.get('entries')
    names = ('stockings', 'footwear', 'sources', 'sourceSets', 'results', 'responses')
    if (not isinstance(tables, dict) or set(tables) != set(names)
            or any(not isinstance(tables[n], list) or len(tables[n]) > MAX_TABLE for n in names)
            or not isinstance(metadata, dict) or {'schema', 'entries', '_storage'} & set(metadata)
            or metadata.get('generatorVersion') != value.get('generatorVersion')
            or not isinstance(links, list) or len(links) > MAX_PAIRS
            or value.get('entryColumns') != ['stocking', 'footwear', 'result', 'sourceSet', 'response']):
        raise ValueError('invalid candidate dictionaries/metadata')
    for name, fields in (('stockings', STOCK), ('footwear', SHOE)):
        if any(not isinstance(r, dict) or set(r) != set(fields)
               or any(not isinstance(r[k], str) for k in fields) for r in tables[name]):
            raise ValueError('invalid candidate identity table')
    if any(not isinstance(r, dict) or SPECIAL & set(r) for r in tables['results']):
        raise ValueError('invalid candidate result table')
    if any(not isinstance(r, dict) for r in tables['responses']):
        raise ValueError('invalid response table')
    if any(not isinstance(r, dict) for r in tables['sources']):
        raise ValueError('invalid candidate source table')

    def index(n, table):
        if type(n) is not int or not 0 <= n < len(table):
            raise ValueError('candidate dictionary reference out of range')
        return table[n]

    sizes = {n: [len(canonical(r).encode('utf-8')) for r in tables[n]] for n in names}
    for group in tables['sourceSets']:
        if not isinstance(group, list) or len(group) > 256:
            raise ValueError('invalid candidate source set')
        for n in group: index(n, tables['sources'])
    expanded = 0; rows = []
    for i, link in enumerate(links):
        if not isinstance(link, list) or len(link) != 5:
            raise ValueError('invalid candidate link')
        st, sh, result, source_set, response_id = link
        stock = index(st, tables['stockings']); shoe = index(sh, tables['footwear'])
        detail = index(result, tables['results'])
        response = None if response_id is None else index(response_id, tables['responses'])
        source_ids = None if source_set is None else index(source_set, tables['sourceSets'])
        expanded += sizes['stockings'][st] + sizes['footwear'][sh] + sizes['results'][result]
        if response_id is not None: expanded += sizes['responses'][response_id]
        if source_ids is not None: expanded += sum(sizes['sources'][n] for n in source_ids)
        if expanded > MAX_EXPANDED:
            raise ValueError('candidate expanded-data limit')
        # A manual edit to one row must not mutate another row sharing a result.
        row = {'index': i, **deepcopy(stock), **deepcopy(shoe), **deepcopy(detail)}
        if response_id is not None: row['stockingResponse'] = deepcopy(response)
        if source_ids is not None:
            row['sources'] = [deepcopy(tables['sources'][n]) for n in source_ids]
        rows.append(row)
    return {'schema': 1, **deepcopy(metadata), 'entries': rows}


def read_candidates(path: Path):
    def pairs(items):
        result = {}
        for k, v in items:
            if k in result: raise ValueError('duplicate JSON key: '+k)
            result[k] = v
        return result
    def bad(text): raise ValueError('nonfinite JSON: '+text)
    data = bounded_read(path, LIMIT)
    value = json.loads(data.decode('utf-8-sig'), object_pairs_hook=pairs, parse_constant=bad)
    report=unpack(value)
    report["_storage"]={"sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data),"path":str(path)}
    return report


def write_candidates(path: Path, report):
    value = pack(report)
    result = stream_json(path, value, compact=True)
    result.update(format=FORMAT, pairs=len(report['entries']),
                  tableCounts={k: len(v) for k, v in value['tables'].items()})
    report['_storage'] = result
    report['counts'] = value['metadata']['counts']
    return result
