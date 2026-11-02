# Building native ICE peer transport

The optional GameNetworkingSockets backend lets world clients negotiate a direct
UDP connection through NAT. The game server carries rendezvous messages and
remains the terrain source when a peer path cannot be established. The existing
TCP peer transport remains available when this optional backend is disabled.

Quiver pins Valve's GameNetworkingSockets revision
[`d534c19aa760df3fb75fd20db13ba1932b8a5463`](https://github.com/ValveSoftware/GameNetworkingSockets/tree/d534c19aa760df3fb75fd20db13ba1932b8a5463).
Its native ICE client is enabled with `ENABLE_ICE=ON`; WebRTC is unnecessary.
See the upstream [P2P documentation](https://github.com/ValveSoftware/GameNetworkingSockets/blob/d534c19aa760df3fb75fd20db13ba1932b8a5463/README_P2P.md).

## Linux and macOS

Install CMake, Ninja, Python 3.12 or newer, a C++ compiler, OpenSSL development
headers, protobuf development libraries, and the matching `protoc` compiler.
For Debian/Ubuntu the additional packages are `cmake ninja-build libssl-dev
libprotobuf-dev protobuf-compiler`; on macOS use Homebrew's `cmake ninja openssl@3
protobuf`. The bootstrap checks for tools but never installs system packages.

From the repository root:

```sh
python3 Misc/build_gns.py
meson configure build -Duse_gns=enabled
ninja -C build
```

For a fresh build, use `meson setup build -Duse_gns=enabled` instead of
`meson configure`. The bootstrap verifies the pinned source archive's SHA-256,
builds a shared library with native ICE, and installs into `.deps/gns`. Downloads,
sources and intermediate objects stay in ignored `.deps/` directories. Repeating
the command reuses that local build. It does not fetch WebRTC or change globally
installed libraries.

`use_gns=auto` is the default: enable the backend when the dependency is found,
otherwise compile the TCP/server fallback. `-Duse_gns=enabled` makes a missing
library a configuration error, and `-Duse_gns=disabled` explicitly selects the
fallback. Set `-Dgns_prefix=/absolute/install/prefix` for another installation, or
`-Dgns_prefix=` for normal system CMake package discovery. The selected library
must be built with `ENABLE_ICE=ON`; older prebuilt packages may omit ICE.

The build embeds the library search path for the selected shared library.
Distributions must package that library and its protobuf/OpenSSL dependencies;
the checkout's `.deps` path is a development installation, not a portable bundle.

For GNU Make:

```sh
make -C Quake GNS_PREFIX="$PWD/.deps/gns" -j
```

Omit `GNS_PREFIX` to build the fallback. Cross-compilation requires a prefix built
for the target architecture, not the host bootstrap output.

## Windows / Visual Studio

Build the same pinned upstream revision with CMake, native ICE, shared libraries,
and protobuf/OpenSSL dependencies for the project's architecture. The bootstrap
can run from a Visual Studio developer shell when `cmake`, `ninja`, and `protoc`
are available; use `--cmake-arg=-DCMAKE_TOOLCHAIN_FILE=...` for a vcpkg toolchain.
Set the MSBuild property `GNSDir` to the resulting install prefix:

```text
msbuild Windows\VisualStudio\vkquake.vcxproj /p:Configuration=Release /p:Platform=x64 /p:GNSDir=C:\dependencies\gns
```

The project adds the public headers and import library and copies
`GameNetworkingSockets.dll` beside the executable. Deploy its matching
protobuf/OpenSSL runtime DLLs there as well. Leaving `GNSDir` unset builds the
TCP/server fallback. MinGW builds accept `GNS_PREFIX` but require a compatible
MinGW-built import library and runtime DLLs.

The library provides ICE negotiation and encrypted reliable transport. Signaling,
peer authorization, authoritative terrain validation and fallback remain game
responsibilities. Native ICE cannot traverse every NAT/firewall combination;
blocked UDP and unsupported mappings fall back to the authoritative game server.

## Runtime controls

`world_peer_nat 1` is enabled by default in capable clients. Set it to `0` to
close the ICE connections and stop NAT signaling; TCP and server terrain delivery
remain available. Re-enabling it renegotiates the peer directory.

The server advertises `world_peer_stun`, defaulting to
`stun.l.google.com:19302`. It accepts a comma-separated GNS STUN server list up to
255 bytes. Set this before clients connect; an empty string disables STUN public
address discovery while retaining local ICE candidates. No STUN credentials are
required. STUN discovers UDP mappings; it does not carry terrain payloads.
This implementation uses the game server's normal terrain delivery when direct
negotiation fails, without a separate TURN service or Valve relay dependency.

The game server assigns peer identities and relays signaling only between live,
mutually introduced clients in the current world epoch. Signals are capped at
8 KiB and fragmented below the game's 1 KiB client message buffer, with rate,
queue, reassembly and expiry limits. GNS callbacks enqueue signaling under a lock;
engine networking and object providers run on the main thread. Object requests
still require expiring session authorization, and the receiver verifies the
Merkle proof and SHA-256 content before installing any terrain.

`world_stats` shows NAT connections, served/fetched objects and failed attempts.
The adapter reuses idle connections for up to 30 seconds and caps outstanding
requests, accepted connections, receive queues and object sizes. ICE gets a
bounded negotiation window; failure cannot prevent authoritative server delivery.
Game traffic continues to use the existing Quake transport. Clients and server
must all use world protocol version 5.

## Verification

```sh
meson test -C build --print-errorlogs
python3 tests/run_world_nat_smoke.py --fixture build/world_peer_nat_fixture
python3 tests/run_world_signal_smoke.py --binary build/vkquake --basedir /path/to/quake
python3 tests/run_world_peer_smoke.py --binary build/vkquake --basedir /path/to/quake --require-nat
```

The NAT fixture uses real ICE sockets in isolated Linux network namespaces, with
two private networks, separate full-cone NAT routers with expiring mappings
created by outbound traffic, and a local STUN responder. It verifies connection
reuse, port-restricted hole punching, revoked authorization, session isolation,
and failure when peer UDP is blocked. Forged and malformed signaling identities
are rejected both before and after a connection is established.
It requires unprivileged user/network namespace support and `ip`/`nft` tools.
No host firewall rules or interfaces are changed. The engine signal test verifies
source identity and recipient admission; the graphical peer test exercises the
actual game integration and authoritative fallback.
