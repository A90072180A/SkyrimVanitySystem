#!/usr/bin/env python3
"""Read-only MO2/Python file visibility comparison; run through MO2.

python vha_io_probe.py --data "D:/.../Skyrim Special Edition/Data"
    --plugin "unofficial skyrim special edition patch.esp" --output report.json
No plugins, meshes or configuration are changed. The optional JSON report is
written only at the explicitly selected output location.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path, PureWindowsPath
import platform
import sys
from vha_fileio import absolute_path, handle_stamp


def probe(path: Path) -> dict:
    result = {'path': str(path)}
    try:
        info = path.stat()  # Diagnostic comparison only. Never gates an open.
        result['pathStat'] = {'ok': True, 'size': info.st_size}
    except OSError as exc:
        result['pathStat'] = {'ok': False, 'error': str(exc)}
    try:
        with path.open('rb') as stream:
            stamp = handle_stamp(stream)
            signature = stream.read(4)
        result['openAndFstat'] = {'ok': True, 'size': stamp[0], 'first4Hex': signature.hex()}
    except OSError as exc:
        result['openAndFstat'] = {'ok': False, 'error': str(exc)}
    result['statFalseNegative'] = not result['pathStat']['ok'] and result['openAndFstat']['ok']
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--plugin', default='unofficial skyrim special edition patch.esp')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    name = args.plugin
    if PureWindowsPath(name).name != name or '/' in name or '\\' in name or ':' in name:
        parser.error('--plugin must be a filename, not a path')
    data = absolute_path(args.data)
    probes = [probe(data / 'Skyrim.esm'), probe(data / name)]
    names = []
    listing_error = None
    try:
        with os.scandir(data) as items:
            names = [entry.name for entry in items if entry.name.casefold() == name.casefold()]
    except OSError as exc:
        listing_error = str(exc)
    # A case-preserving spelling from this SAME virtual directory is diagnostic;
    # never search inactive mods or copy another provider into Data.
    if len(names) == 1 and names[0] != name:
        probes.append(probe(data / names[0]))
    usvfs = None
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        api = ctypes.WinDLL('kernel32', use_last_error=True).GetModuleHandleW
        api.argtypes = [wintypes.LPCWSTR]
        api.restype = wintypes.HMODULE
        usvfs = {n: bool(api(n)) for n in ('usvfs_x64.dll', 'usvfs_x86.dll')}
    report = {'schema': 1, 'purpose': 'open/fstat versus path stat; no scan performed',
              'python': sys.version, 'executable': sys.executable,
              'windows': platform.platform(), 'usvfsModules': usvfs,
              'data': str(data), 'directoryMatches': names,
              'directoryError': listing_error, 'files': probes}
    output = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    print(output)
    if args.output:
        # Explicit diagnostic destination only; do not overwrite a config file.
        if args.output.suffix.casefold() != '.json' or args.output.name.casefold().startswith('vanityubeheeladapter'):
            parser.error('use a separate diagnostic .json filename')
        with args.output.open('x', encoding='utf-8') as stream:
            stream.write(output)
    return 0 if probes[1]['openAndFstat']['ok'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
