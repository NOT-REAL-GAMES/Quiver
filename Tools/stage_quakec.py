#!/usr/bin/env python3
"""Stage the bundled QuakeC source tree next to built tools."""
import pathlib
import shutil
import sys

source, destination, stamp = map(pathlib.Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for item in source.iterdir():
    if item.is_file():
        shutil.copy2(item, destination / item.name)
stamp.write_text('id1 QuakeC 1.01\n')
