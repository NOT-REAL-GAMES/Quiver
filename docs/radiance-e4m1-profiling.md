# E4M1 Radiance profiling

`gpu_stage_time start` / `gpu_stage_time stop` optionally report completed-frame
GPU timestamp intervals around the world and opaque-entity secondary-command
groups. These ranges include all work in those groups, and may overlap other
work; do not add them to each other or to the Radiance prepass timing. The
world range follows whichever world draw path is selected, including the
experimental half-resolution and checkerboard receiver paths. It does not
include their separate setup, tracing, or reconstruction passes, and it is not
an exclusive measurement of Radiance lighting. The existing `gpu_frame_time`
reports whole frames. Query results are retrieved
without waiting when the relevant frame slot's fence is complete; if any
timestamps are unavailable, that sample is discarded.

`tests/bench_radiance.py --gpu-stages` records these ranges in each result.
Radiance warmup now begins only after an active backend has produced a
completed prepass and the static acceleration-structure upload/build has
finished. This prevents timing shader/scene initialization as steady state.
Use `tests/smoke_gpu_stages.py` to exercise the timers through transparency
variants, a video restart, and a worldless menu; `--validation` requires a
validation-capable debug build. `--receiver-modes` additionally requires and
checks active half-resolution and checkerboard receiver paths.

An exploratory run on an RTX 4080, at 1280×720 in Xvfb, used E4M1 camera
`1312 680 688 55 90 0`, ray-query backend, quality 1, and unit light gains.
After readiness and four seconds' warmup, two alternating three-second pairs
gave the following per-run GPU medians in milliseconds:

| Interval | Classic | Radiance |
| --- | --- | --- |
| Whole frame | 0.801 / 1.194 | 11.519 / 12.298 |
| World command group | 0.070 / 0.098 | 7.994 / 9.311 |
| Opaque entity command group | 0.034 / 0.055 | 0.381 / 0.433 |

The separately sampled prepass medians were 0.780 and 0.722 ms; reported
static scene uploads and static BLAS builds were zero at the snapshots. This
identifies the world rendering group as a major area to investigate, not a
precise isolation of a particular shader or proof of a BVH rebuild. Profiling
adds timestamp queries, and the Xvfb setup showed clock and pacing variation:
median graphics clocks were 345/240 MHz in classic windows and 1365/1095 MHz
in Radiance windows. These are diagnostic GPU intervals, not representative
native-display FPS measurements. The complete results and timestamped clock
samples for this local run are `build/radiance-stage-e4m1/results.json` and
`build/radiance-stage-clocks.csv`.

An additional exploratory shaft run tested a conservative per-fragment box
rejection before point-light distance calculations and emissive-rectangle
basis calculations. The baseline and candidate used the same renderer and
scene apart from that shader change. With the same resolution, pose, backend,
quality, and warmup described above, one three-second sample of each gave
world-group GPU medians of 6.771 ms baseline and 7.075 ms candidate, and
whole-frame GPU medians of 9.819 ms and 10.078 ms respectively. One pair does
not establish a statistically reliable regression; it provides no evidence
for retaining this optimization. The shader experiment was reverted. Results:
`build/broadphase-test-base3/results.json` and
`build/broadphase-test-candidate/results.json`.

An additional diagnostic disabled portions of receiver shading in isolated
shader builds that included the rejected broad-phase experiment above. One
sample each gave world-group GPU medians of 6.307 ms for
the full path, 0.941 ms without direct-light evaluation, and 6.611 ms without
probe interpolation. These measurements identify direct-light evaluation as
the leading investigation target; the intervals are non-additive and affected
by clock variation.

A separate experiment added conservative per-cell facing exclusions for
axis-aligned receivers. In a baseline/candidate/baseline sequence, the
world-group medians were 6.771 / 8.205 / 8.048 ms and whole-frame medians were
9.819 / 11.645 / 11.265 ms. These samples show no repeatable improvement and
the experiment was reverted. The final two results are in
`build/facing-test-candidate/results.json` and `build/facing-test-base/results.json`;
the first is the broad-phase baseline above.

Another isolated diagnostic kept direct-light arithmetic but forced all
receiver shadow tests visible. A single matched baseline/diagnostic pair gave
world-group medians of 8.525 / 3.115 ms and whole-frame medians of 11.496 /
5.387 ms. This is not a usable renderer mode: it deliberately removes shadows.
It identifies receiver visibility as a substantial cost to investigate, subject
to the same timing variability. Results are in
`build/shadowcost-test-base/results.json` and
`build/shadowcost-test-noshadow/results.json`.
