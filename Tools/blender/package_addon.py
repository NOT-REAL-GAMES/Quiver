#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package the Blender exporter without requiring Blender or third-party modules."""

import argparse
from pathlib import Path
import sys
import zipfile


def build_archive(output: Path, *, legacy: bool = False) -> Path:
    """Build an extension ZIP, or a traditional add-on ZIP with ``legacy=True``."""
    tool_dir = Path(__file__).resolve().parent
    source_dir = tool_dir / "io_export_quiver_md5"
    required = [source_dir / name for name in (
        "__init__.py", "exporter.py", "model.py", "md5.py", "baking.py", "geometry.py",
        "sizing.py", "bake_cache.py", "bake_inputs.py", "bake_manager.py",
    )]
    if not legacy:
        required.append(source_dir / "blender_manifest.toml")
    for path in required:
        if not path.is_file():
            raise FileNotFoundError(f"Missing add-on source: {path}")

    entries = []
    for path in sorted(source_dir.rglob("*")):
        relative = path.relative_to(source_dir)
        if (
            not path.is_file()
            or path.is_symlink()
            or any(part.startswith(".") or part == "__pycache__" for part in relative.parts)
            or path.suffix in {".pyc", ".pyo"}
        ):
            continue
        archive_path = Path(source_dir.name) / relative if legacy else relative
        entries.append((path, archive_path.as_posix()))

    readme = tool_dir / "README.md"
    if readme.is_file():
        archive_readme = f"{source_dir.name}/README.md" if legacy else "README.md"
        entries.append((readme, archive_readme))
    license_file = tool_dir.parents[1] / "LICENSE.txt"
    if license_file.is_file():
        archive_license = f"{source_dir.name}/LICENSE.txt" if legacy else "LICENSE.txt"
        entries.append((license_file, archive_license))

    output = output.expanduser().resolve()
    # Avoid accidentally replacing add-on sources with their own archive.
    if output in (Path(__file__).resolve(), readme, license_file) or output.is_relative_to(source_dir):
        raise ValueError("Choose an output path outside the add-on source directory.")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, name in sorted(entries, key=lambda entry: entry[1]):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return output


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/tmp/quiver-md5-exporter.zip"),
        help="destination ZIP (default: /tmp/quiver-md5-exporter.zip)",
    )
    parser.add_argument(
        "--legacy",
        action="store_true",
        help="wrap files in io_export_quiver_md5/ for traditional add-on installation",
    )
    args = parser.parse_args(argv)
    try:
        output = build_archive(args.output, legacy=args.legacy)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Built {'legacy add-on' if args.legacy else 'Blender extension'}: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
