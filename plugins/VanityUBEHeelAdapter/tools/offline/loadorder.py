"""Read-only SSE load-order reconstruction for MO2, not a load-order sorter.

Primary order: five base masters, then installed entries from game-root
Skyrim.ccc in manifest order. Ordinary plugins still require a plugins.txt '*'.
See MO2 GameSkyrimSE::primaryPlugins/CCPlugins and libloadorder game_settings.
All file probes use open/fstat so the offline2 virtual-file fix is retained.
"""
from __future__ import annotations
import hashlib
from pathlib import Path, PureWindowsPath
from vha_fileio import absolute_path, optional_bytes, readable_file

BASE_MASTERS = ('Skyrim.esm', 'Update.esm', 'Dawnguard.esm',
                'HearthFires.esm', 'Dragonborn.esm')
TEXT_LIMIT = 1024 * 1024


def _names(payload: bytes | None, path: Path, starred=False) -> list[str]:
    from .formats import FormatError
    if payload is None:
        return []
    try:
        lines = payload.decode('utf-8-sig').splitlines()
    except UnicodeDecodeError as exc:
        raise FormatError(f'invalid-list-encoding: {path}; expected UTF-8') from exc
    names = []
    for number, line in enumerate(lines, 1):
        name = line.strip()
        if not name or name.startswith(('#', ';')):
            continue
        enabled = name.startswith('*')
        if enabled:
            name = name[1:].strip()
        p = PureWindowsPath(name)
        if (not name or p.name != name or p.drive or any(c in name for c in '\\/:\x00')
                or any(ord(c) < 32 for c in name)
                or p.suffix.casefold() not in ('.esm', '.esp', '.esl')):
            raise FormatError(f'unsafe-plugin-name: {path}:{number}: {name!r}')
        if not starred or enabled:
            names.append(name)
    return names


def resolve_plugins(data: Path, profile: Path, *, evidence=None) -> list[str]:
    from .formats import FormatError
    data, profile = absolute_path(data), absolute_path(profile)
    report = evidence if evidence is not None else {}
    report.update(state='reading-inputs', cccPath=str(data.parent/'Skyrim.ccc'),
                  inputSources=[], includedCCC=[], absentImplicit=[],
                  entries=[], warnings=[])

    def read_list(path: Path, required=False):
        payload = optional_bytes(path, TEXT_LIMIT)
        report['inputSources'].append({
            'kind':'load-order-input', 'path':str(path), 'exists':payload is not None,
            'sha256':hashlib.sha256(payload).hexdigest() if payload is not None else None})
        if required and payload is None:
            raise FormatError(f'missing-profile-list: {path}')
        return payload

    plugins_payload = read_list(profile/'plugins.txt', required=True)
    order_payload = read_list(profile/'loadorder.txt')
    ccc_payload = read_list(data.parent/'Skyrim.ccc')
    report['cccState'] = 'present' if ccc_payload is not None else 'absent'
    starred = _names(plugins_payload, profile/'plugins.txt', starred=True)
    order = _names(order_payload, profile/'loadorder.txt')
    ccc = _names(ccc_payload, data.parent/'Skyrim.ccc')
    report['cccEntries'] = ccc
    report['starredEntries'] = starred
    report['profileOrder'] = order
    if ccc_payload is None:
        report['warnings'].append('Skyrim.ccc absent: no extra implicit plugins inferred; '
                                  'ordinary unstarred or arbitrary cc* files stay excluded.')

    # Keep the spelling used by the profile, but identities are case-insensitive.
    spellings = {}
    for name in [*starred, *order, *BASE_MASTERS, *ccc]:
        spellings.setdefault(name.casefold(), name)
    result, seen, declared = [], set(), set()
    for source, names in (('base-master', BASE_MASTERS), ('Skyrim.ccc', ccc)):
        for original in names:
            key = original.casefold()
            if key in declared:
                continue
            declared.add(key)
            name = spellings[key]
            if not readable_file(data/name):
                report['absentImplicit'].append({'name':name, 'source':source})
                # An installed CCC entry can alter all winners and BSA priority.
                report['inputSources'].append({'kind':'absent-implicit-plugin',
                    'path':str(data/name), 'exists':False, 'sha256':None})
                continue
            result.append(name)
            seen.add(key)
            report['entries'].append({'name':name, 'source':source})
            if source == 'Skyrim.ccc':
                report['includedCCC'].append(name)

    # Do not activate dependencies merely because they exist. Do not repair an
    # ordinary late master: the caller's dependency validation must reject it.
    active = {name.casefold() for name in starred}
    for name in [*order, *starred]:
        key = name.casefold()
        if key in active and key not in seen:
            result.append(spellings[key])
            seen.add(key)
            report['entries'].append({'name':spellings[key], 'source':'plugins.txt-star'})
    if not result:
        raise FormatError('empty-active-load-order: check Data/profile')
    for index, entry in enumerate(report['entries']):
        entry['index'] = index
    report.update(state='resolved', effectiveOrder=result,
                  semantics='Read-only: installed primary masters + Skyrim.ccc order + '
                  'starred ordinary plugins in profile order. No implicit cc* wildcard, '
                  'no automatic master activation, no profile edits.')
    return result


def verify_sources(sources) -> None:
    """Invalidate suggestions if lists change, or previously absent inputs appear."""
    from .formats import file_hash
    for source in sources:
        path = Path(source['path'])
        if source.get('exists') is False:
            if readable_file(path):
                raise ValueError('input appeared; rescan: ' + str(path))
        else:
            try:
                digest = file_hash(path)
            except FileNotFoundError as exc:
                raise ValueError('input disappeared; rescan: ' + str(path)) from exc
            if digest != source['sha256']:
                raise ValueError('input changed; rescan: ' + str(path))
