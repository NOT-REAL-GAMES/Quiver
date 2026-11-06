# Static MD5 shadow casters

MD5 models previously received lighting but were omitted from the Radiance
scene: the retained alias geometry and caster checks accepted only MDL/MD3.
Normal-based shading on an MD5 did not establish that it cast or self-occluded.

Single-frame `PV_MD5` and `PV_MD5_8` surfaces now retain owned float positions,
UVs and indices. Positions use the renderer's packed, normalized joint weights
and frame-zero matrices, including nonidentity joints. They are not decoded
from the bind-position prefix or quantized to MD3 coordinates. Every surface
must be supported and have valid material data before the model is accepted;
failure rolls back its complete append. Animated MD5 remains unsupported.

The actual Sponza export has one frame, one root joint, 26 surfaces, 434,364
vertices and 262,266 triangles. Production extraction preserves every position
and UV exactly, uses 10,263,164 retained bytes, and releases all owned memory.
The expanded triangle payload is 25,177,536 bytes per copy. CPU tests cover
four/eight weights, malformed input, normalized byte sums, vertex/index limits,
ownership, cache invalidation and transactional rollback; ASan/UBSan pass.

The exact material atlas ceiling is raised from 128 to 512 MiB. It is a limit,
not an eager allocation: ordinary maps retain their actual small atlases.
Sponza's 25 distinct 2048×2048 RGBA textures require about 400 MiB of decoded
pixels and 432 MiB per packed atlas in the DM2 test. Both frame slots and upload staging
make this materially larger than a normal Quake map. Texture detail and cutout
alpha are preserved rather than silently downscaled.

Actual-asset hardware and software tests use identical fixed DM2 lighting and
camera with entities hidden in both control images. Adding the hidden Sponza
caster darkens 24,602 world pixels by more than 4/255, with maximum change 61;
removing it restores the image within 2/255. The two backends' images match
exactly. Both exit cleanly with no SDK synchronization, Vulkan or driver errors.
This proves cast shadows independently of drawing/material shading on Sponza.

The initial correct prototype exposed full-geometry fingerprinting and scene
assembly costs: held-clock CPU preparation rose to roughly 59–68 ms. Per-entity
bounds/content summaries now avoid hashing unchanged meshes. Stable scene
ranges compare only freshly extracted entity geometry, patch only changed
ranges, and retain byte-exact rollback. Identical output from a new animation
revision commits without a scene upload or BVH refit. Reorders and topology
changes retain the general assembly path; positions are still validated.

A normal animation clock also exposed a more expensive path: a small animated
brush texture redecoded, packed and uploaded all 400 MiB of Sponza textures,
taking about 1.4 seconds per frame. Primary/alternate brush animation frames
are now packed once; animation changes only the small logical material table.
Source reloads, inactive frames, fullbrights and registry compaction remain
tracked, with separate pixel-upload and logical-material revisions. The cache
can retain unused packed pixels after compaction until the next inventory
rebuild or map change; it stays within the atlas ceiling.

Final prototype `44cfcca0` passed actual-asset tests on both tracing backends.
Held-clock preparation measured 0.308 ms hardware / 0.332 ms software, with no
assembly copies and only 24 small alias triangles checked. Normal-clock
preparation measured roughly 20–26 ms; existing full BVH refits and emissive
scene processing remain significant when small geometry actually moves. These
are sampled CPU preparation costs, not gameplay FPS. Pixel-atlas revision
remained fixed as logical animation revisions advanced, and all shadow images
retained the same cast/removal evidence. Both runs exited cleanly with no SDK,
synchronization or driver errors.

A separate CPU fixture with 262,266 cached triangles plus 24 animated triangles
reduced small metadata updates from 18.12 to 0.0025 ms and position updates
from 24.67 to 4.13 ms. Optimized builds and ASan/UBSan cover malformed fresh
arrays, exact equality, derived flags, material/UV changes, reorders and failed
refit rollback.

Evidence is retained in
[`radiance-md5-static-20261105/actual-asset-report.json`](../build/radiance-md5-static-20261105/actual-asset-report.json)
and
[`radiance-static-md5-20261105/final-runtime`](../build/radiance-static-md5-20261105/final-runtime/),
with isolated CPU timings in
[`cpu-runtime-profile/report.json`](../build/radiance-static-md5-20261105/cpu-runtime-profile/report.json).
