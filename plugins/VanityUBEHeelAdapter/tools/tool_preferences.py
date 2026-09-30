"""Local UI path memory. Never scans files, changes game config or affects CLI jobs."""
from __future__ import annotations
import json
import os
from pathlib import Path
import tempfile
from vha_fileio import optional_bytes, ensure_directory, unlink_missing_ok

PATH_KEYS = ('data', 'profile', 'mods_root', 'output', 'plugins', 'user_file')
SCOPED_KEYS = ('output', 'plugins', 'user_file')
LIMIT = 256 * 1024


def settings_path() -> Path:
    override = os.environ.get('VHA_TOOL_SETTINGS')
    if override:
        return Path(override).expanduser()
    folder = os.environ.get('LOCALAPPDATA') or os.environ.get('XDG_CONFIG_HOME')
    return (Path(folder) if folder else Path.home() / '.config') / 'VanityUBEHeelAdapter' / 'tools-paths.json'


def profile_key(value: str) -> str:
    # Lexical only: paths in the MO2 virtual namespace need not pass stat().
    return value.replace('\\', '/').rstrip('/').casefold()


class Preferences:
    def __init__(self, path: Path | None = None):
        self.path = path or settings_path()
        self.warning = ''
        self.document = {'schema': 1, 'last': {}, 'profiles': {}}
        try:
            raw = optional_bytes(self.path, LIMIT)
            if raw is not None:
                doc = json.loads(raw.decode('utf-8-sig'))
                if not isinstance(doc, dict) or doc.get('schema') != 1:
                    raise ValueError('unsupported path-memory schema')
                for key in ('last', 'profiles'):
                    if not isinstance(doc.get(key), dict):
                        raise ValueError('invalid path-memory structure')
                rows = [doc['last'], *doc['profiles'].values()]
                if len(rows) > 129 or any(not isinstance(row, dict) or
                    any(k not in PATH_KEYS or not isinstance(v, str) or len(v) > 32768 or '\0' in v
                        for k, v in row.items()) for row in rows):
                    raise ValueError('invalid path-memory values')
                self.document = doc
        except (OSError, ValueError, UnicodeError) as exc:
            # Damaged preferences do not prevent opening the tool. Do not destroy
            # their bytes automatically: user can explicitly save to repair them.
            self.warning = f'路径记忆未读取：{exc}。请重新选择路径并点击“记住路径”。'

    def initial(self, args) -> dict[str, str]:
        last = self.document['last']
        result = {k: str(getattr(args, k, None) or last.get(k, '')) for k in PATH_KEYS}
        current_profile = result['profile']
        scoped = self.for_profile(current_profile)
        for key in SCOPED_KEYS:
            explicit = getattr(args, key, None)
            result[key] = str(explicit) if explicit is not None else scoped.get(key, '')
        # No implicit cross-game reuse of physical write destinations.
        explicit_data = getattr(args, 'data', None)
        if explicit_data is not None and profile_key(str(explicit_data)) != profile_key(last.get('data', '')):
            for key in SCOPED_KEYS:
                if getattr(args, key, None) is None:
                    result[key] = ''
        return result

    def for_profile(self, profile: str) -> dict[str, str]:
        return dict(self.document['profiles'].get(profile_key(profile), {}))

    def save(self, values: dict[str, str], *, repair: bool = False) -> None:
        if self.warning and not repair:
            raise ValueError(self.warning)
        values = {k: str(values.get(k, '')).strip() for k in PATH_KEYS}
        if any(len(v) > 32768 or '\0' in v for v in values.values()):
            raise ValueError('无效路径')
        # Merge profiles from another window rather than overwriting its entries.
        latest = Preferences(self.path)
        if latest.warning and not repair:
            raise ValueError(latest.warning)
        profiles = dict(latest.document['profiles'])
        key = profile_key(values['profile'])
        if key:
            profiles.pop(key, None)
            profiles[key] = {k: values[k] for k in SCOPED_KEYS}
        while len(profiles) > 128:
            profiles.pop(next(iter(profiles)))
        doc = {'schema': 1, 'last': values, 'profiles': profiles}
        payload = (json.dumps(doc, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
        if len(payload) > LIMIT:
            raise ValueError('路径记忆文件超过上限')
        ensure_directory(self.path.parent)
        fd, temporary = tempfile.mkstemp(prefix=self.path.name + '.', suffix='.tmp', dir=self.path.parent)
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(payload); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        finally:
            unlink_missing_ok(Path(temporary))
        self.document = doc
        self.warning = ''
