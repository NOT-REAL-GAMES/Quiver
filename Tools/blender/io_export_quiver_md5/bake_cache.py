# SPDX-License-Identifier: GPL-2.0-or-later
"""Content-addressed disk cache for finished bakes. Needs no Blender.

An entry's directory is named after the hash of everything that determines its
pixels, so editing an input moves to a new key rather than overwriting anything.
Each file's size and digest are checked on every lookup: a damaged entry is
discarded and rebaked. Entries appear by atomic rename, so a crash or a second
Blender process cannot expose a half-written one.
"""

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

FORMAT = 1
MANIFEST = 'entry.json'
DEFAULT_LIMIT = 4 * 1024 ** 3  # Least recently used entries go past this.
_KEY = re.compile(r'[0-9a-f]{64}')
_STALE_STAGING_SECONDS = 24 * 60 * 60


@dataclass(frozen=True)
class CacheEntry:
    key: str
    directory: Path
    files: tuple
    meta: dict

    def path(self, name):
        return self.directory / name if name in self.files else None


def default_directory():
    """Per-user cache shared by every project, kept out of game directories."""
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local'
    elif sys.platform == 'darwin':
        base = Path.home() / 'Library' / 'Caches'
    else:
        base = os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache'
    return Path(base) / 'quiver_md5' / 'bakes'


def make_key(inputs):
    """Hash JSON-compatible inputs in a form stable across processes."""
    text = json.dumps(inputs, sort_keys=True, separators=(',', ':'), allow_nan=True)
    return hashlib.blake2b(text.encode('utf-8'), digest_size=32).hexdigest()


def file_digest(path):
    digest = hashlib.blake2b(digest_size=32)
    with open(path, 'rb') as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


class BakeCache:
    def __init__(self, root, limit=DEFAULT_LIMIT):
        self.root = Path(root)
        self.limit = limit
        self.problems = []  # Discarded entries, for the export report.

    @property
    def _base(self):
        return self.root / f'v{FORMAT}'

    def _directory(self, key):
        if not _KEY.fullmatch(key):
            raise ValueError(f"Invalid bake cache key: {key!r}")
        return self._base / key[:2] / key

    def _read(self, key, directory):
        """Return a verified entry; raise ValueError or OSError if it is unusable."""
        manifest = directory / MANIFEST
        if manifest.stat().st_size > 1024 * 1024:
            raise ValueError("oversized manifest")
        data = json.loads(manifest.read_text(encoding='utf-8'))
        if data['format'] != FORMAT or data['key'] != key:
            raise ValueError("manifest does not match its key")
        files = data['files']
        for name, record in files.items():
            if Path(name).name != name or name == MANIFEST:
                raise ValueError(f"invalid file name {name!r}")
            path = directory / name
            if path.stat().st_size != record['size'] or file_digest(path) != record['digest']:
                raise ValueError(f"{name} does not match its recorded digest")
        return CacheEntry(key, directory, tuple(sorted(files)), data['meta'])

    def lookup(self, key):
        directory = self._directory(key)
        if not directory.exists():
            return None
        try:
            entry = self._read(key, directory)
        except (KeyError, TypeError, AttributeError, ValueError, OSError) as exc:
            self.problems.append(f"{key[:12]}: {exc}")
            shutil.rmtree(directory, ignore_errors=True)
            return None
        try:
            os.utime(directory / MANIFEST)  # Mark as recently used.
        except OSError:
            pass
        return entry

    def put(self, key, files, meta):
        """Copy ``{name: path}`` into the cache, replacing any older entry."""
        target = self._directory(key)
        target.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.tmp-', dir=self._base))
        try:
            records = {}
            for name, source in files.items():
                shutil.copyfile(source, staging / name)
                records[name] = {'size': (staging / name).stat().st_size, 'digest': file_digest(staging / name)}
            manifest = {'format': FORMAT, 'key': key, 'files': records, 'meta': meta}
            (staging / MANIFEST).write_text(json.dumps(manifest, indent=1, sort_keys=True) + '\n', encoding='utf-8')
            if target.exists():
                shutil.rmtree(target)
            try:
                os.rename(staging, target)
            except OSError:
                # Another process published the same key first; its bytes come
                # from identical inputs. Anything else is a real failure.
                if not target.exists():
                    raise
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        return self._read(key, target)

    def _entries(self):
        for base in self.root.glob('v[0-9]*'):
            for prefix in base.iterdir() if base.is_dir() else ():
                if prefix.is_dir() and re.fullmatch(r'[0-9a-f]{2}', prefix.name):
                    for directory in prefix.iterdir():
                        if directory.is_dir() and _KEY.fullmatch(directory.name):
                            yield directory

    @staticmethod
    def _size(directory):
        return sum(path.stat().st_size for path in directory.iterdir() if path.is_file())

    def usage(self):
        """Return ``(entries, bytes)`` currently stored."""
        directories = list(self._entries())
        return len(directories), sum(self._size(directory) for directory in directories)

    def evict(self, keep=()):
        """Drop least recently used entries over the limit, sparing ``keep``."""
        now = time.time()
        for base in self.root.glob('v[0-9]*'):
            for staging in base.glob('.tmp-*'):
                if now - staging.stat().st_mtime > _STALE_STAGING_SECONDS:
                    shutil.rmtree(staging, ignore_errors=True)
        entries = []
        for directory in self._entries():
            manifest = directory / MANIFEST
            used = manifest.stat().st_mtime if manifest.exists() else 0
            entries.append((used, directory, self._size(directory)))
        total = sum(entry[2] for entry in entries)
        removed = 0
        for _, directory, size in sorted(entries, key=lambda entry: entry[0]):
            if total <= self.limit:
                break
            if directory.name not in keep:
                shutil.rmtree(directory, ignore_errors=True)
                total -= size
                removed += 1
        return removed

    def clear(self):
        """Delete cache entries only, even if the root holds unrelated files."""
        directories = list(self._entries())
        for directory in directories:
            shutil.rmtree(directory)
        for base in self.root.glob('v[0-9]*'):
            for staging in base.glob('.tmp-*'):
                shutil.rmtree(staging, ignore_errors=True)
        return len(directories)
