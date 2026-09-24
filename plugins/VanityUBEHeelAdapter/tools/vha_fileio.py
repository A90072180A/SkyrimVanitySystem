"""File access for MO2 child processes, including Python 3.12+.

Never use path-based stat/existence as a prerequisite for reading virtual files.
Open through normal OS hooks first; fstat addresses the resulting file handle.
This module does not monkeypatch pathlib/os or reach into inactive mod folders.
"""
from __future__ import annotations
import os
from pathlib import Path
import stat


def absolute_path(path: Path) -> Path:
    """Lexical absolute path: preserve the virtual namespace, not realpath."""
    return Path(os.path.abspath(os.fspath(path)))


def handle_stamp(stream) -> tuple[int, int, int, int]:
    info = os.fstat(stream.fileno())
    if not stat.S_ISREG(info.st_mode):
        raise OSError("Not a regular file: " + str(stream.name))
    return info.st_size, info.st_mtime_ns, info.st_dev, info.st_ino


def file_stamp(path: Path) -> tuple[int, int, int, int]:
    with path.open("rb") as stream:
        return handle_stamp(stream)


def readable_file(path: Path) -> bool:
    """Only genuine missing-path errors mean absent; access errors propagate."""
    try:
        file_stamp(path)
        return True
    except (FileNotFoundError, NotADirectoryError):
        return False


def optional_bytes(path: Path, limit: int = 16 * 1024 * 1024) -> bytes | None:
    try:
        with path.open("rb") as stream:
            before = handle_stamp(stream)
            payload = stream.read(limit + 1)
            if before != handle_stamp(stream):
                raise OSError("File changed during read: " + str(path))
    except (FileNotFoundError, NotADirectoryError):
        return None
    if len(payload) > limit:
        raise ValueError("File exceeds size limit: " + str(path))
    return payload


def ensure_directory(path: Path) -> None:
    """Create parents without pathlib.mkdir's stat-based exist_ok fallback."""
    path = absolute_path(path)
    try:
        os.mkdir(path)
    except FileNotFoundError:
        if path.parent == path:
            raise
        ensure_directory(path.parent)
        ensure_directory(path)
    except FileExistsError:
        # scandir opens/enumerates a directory. A same-named file still fails.
        with os.scandir(path):
            pass


def unlink_missing_ok(path: Path) -> None:
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
