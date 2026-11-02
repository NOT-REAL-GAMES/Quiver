#!/usr/bin/env python3
"""Build the pinned native-ICE GameNetworkingSockets dependency locally."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

REVISION = "d534c19aa760df3fb75fd20db13ba1932b8a5463"
ARCHIVE_SHA256 = "099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab"
URL = f"https://codeload.github.com/ValveSoftware/GameNetworkingSockets/tar.gz/{REVISION}"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=root / ".deps/gns")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--cmake-arg", action="append", default=[], help="Extra CMake argument; use --cmake-arg=-DNAME=value")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    for program in ("cmake", "ninja", "protoc"):
        if not shutil.which(program):
            parser.error(f"{program} is required; see docs/world-networking.md")
    cache = root / ".deps"
    cache.mkdir(exist_ok=True)
    archive = cache / f"GameNetworkingSockets-{REVISION}.tar.gz"
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        print(f"Downloading pinned GameNetworkingSockets {REVISION}", flush=True)
        with urllib.request.urlopen(URL, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != ARCHIVE_SHA256:
            temporary.unlink()
            raise RuntimeError("GameNetworkingSockets archive checksum mismatch")
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise RuntimeError(f"Corrupt cached archive: {archive}")
    source = cache / f"GameNetworkingSockets-{REVISION}"
    if not source.exists():
        with tarfile.open(archive) as package:
            # The verified archive still uses Python's safe extraction filter.
            package.extractall(cache, filter="data")
    build = cache / "gns-build"
    command = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={args.prefix.resolve()}",
        "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIB=ON", "-DBUILD_STATIC_LIB=OFF",
        "-DENABLE_ICE=ON", "-DUSE_STEAMWEBRTC=OFF", "-DBUILD_EXAMPLES=OFF",
        "-DBUILD_TESTS=OFF", "-DBUILD_TOOLS=OFF", "-DUSE_CRYPTO=OpenSSL",
    ]
    if os.name != "nt":
        command += [f"-DCMAKE_C_COMPILER={os.environ.get('CC', shutil.which('gcc') or 'cc')}",
                    f"-DCMAKE_CXX_COMPILER={os.environ.get('CXX', shutil.which('g++') or 'c++')}"]
    subprocess.run(command + args.cmake_arg, check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)
    licenses = args.prefix.resolve() / "share/licenses/GameNetworkingSockets"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "LICENSE", licenses / "LICENSE")
    print(f"Native ICE dependency installed locally at {args.prefix.resolve()}")
    print("Enable explicitly: meson configure build -Duse_gns=enabled")


if __name__ == "__main__":
    main()
