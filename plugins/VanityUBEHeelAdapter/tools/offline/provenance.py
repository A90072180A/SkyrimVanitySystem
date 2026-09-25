"""Best-effort provenance from the opened Windows file handle, never mods walking."""
from __future__ import annotations
import os
from pathlib import PureWindowsPath


def opened_path(stream):
    """Use the SAME handle used to read bytes. Unknown is not a guessed provider."""
    if os.name != 'nt': return None
    try:
        import ctypes
        import msvcrt
        from ctypes import wintypes
        fn = ctypes.WinDLL('kernel32', use_last_error=True).GetFinalPathNameByHandleW
        fn.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
        fn.restype = wintypes.DWORD
        handle = msvcrt.get_osfhandle(stream.fileno())
        size = 512
        while size <= 32768:
            buf = ctypes.create_unicode_buffer(size)
            n = fn(handle, buf, size, 0)
            if not n: return None
            if n < size: return clean_path(buf.value)
            size = n + 1
    except (OSError, ValueError, AttributeError):
        pass
    return None


def clean_path(value):
    value = str(value).replace('/', '\\')
    if value.startswith('\\\\?\\UNC\\'): return '\\\\' + value[8:]
    if value.startswith('\\\\?\\'): return value[4:]
    return value


def provider_name(physical, mods_root=None, overwrite_root=None):
    if not physical: return None
    path = PureWindowsPath(clean_path(physical))
    if not path.is_absolute(): return None
    for root, label in ((mods_root, None), (overwrite_root, 'Overwrite')):
        if not root: continue
        try:
            relative = path.relative_to(PureWindowsPath(clean_path(root)))
        except ValueError:
            continue
        if label and relative.parts: return label
        if len(relative.parts) >= 2: return relative.parts[0]
    return None


def annotate(row, mods_root=None, overwrite_root=None):
    # Model provenance uses NIF sources, not a separate TRI's provider.
    models = [s for s in row.get('sources', []) if s.get('resource', '').lower().endswith('.nif')]
    paths = sorted({s['physicalPath'] for s in models if s.get('physicalPath')})
    names = {provider_name(p, mods_root, overwrite_root) for p in paths}
    row['modelPhysicalPaths'] = paths
    row['modelProviderMods'] = sorted(n for n in names if n)
    row['modelProviderState'] = ('resolved' if models and all(
        provider_name(s.get('physicalPath'), mods_root, overwrite_root) for s in models)
        else 'partial' if row['modelProviderMods'] else 'unknown')
    return row
