# First-person body motion

`Quake/view_motion.c` replaces the classic sine-wave view bob with a body that breathes and takes steps. It changes only what is drawn: `cl.viewangles`, physics, the server and demos are never touched, so turning it on or off cannot change where shots go or how the player moves. The camera can point up to about 0.6 degrees away from the true aim line when sprinting while spent, in the same way that `v_gunkick` and `cl_rollangle` already do.

## Cvars

All of these are saved with the game config.

| Cvar | Default | Meaning |
| --- | --- | --- |
| `v_bodymotion` | `1` | `0` restores the classic `V_CalcBob` and removes everything below. |
| `v_breath` | `1` | Breathing strength, `0` to `3`. `0` removes breathing. |
| `v_gait` | `1` | Strength of the gait rotation and sideways weight shift, `0` to `3`. `0` keeps only the vertical bob. |
| `v_gait_pitch` | `0.25` | Peak head nod at run speed, degrees. |
| `v_gait_yaw` | `0.3` | Peak shoulder turn at run speed, degrees. |
| `v_gait_roll` | `0.35` | Peak lean at run speed, degrees. |
| `v_gait_sway` | `0.35` | Peak sideways weight shift at run speed, units. |
| `v_exertion_rise` | `10` | Seconds for exertion to cover 63% of the way to its target while moving. |
| `v_exertion_recover` | `7` | The same while recovering. |

The classic cvars keep their meaning:

| Cvar | Effect here |
| --- | --- |
| `cl_bob` | Master strength, relative to the default of `0.02` and limited to 2.5x. `0` gives a completely still view, with no gait and no breathing. |
| `cl_rollangle` | Scales all roll, relative to the default of `2` and limited to 2.5x. `0` removes all roll from the camera and the weapon. |
| `cl_bobcycle` | Footfall period. The default of `0.6` gives about 2.3 steps a second at run speed and 1.8 at walking speed. `0` stops the gait but not the breathing. |
| `cl_bobup` | Share of each step spent rising, as with the classic bob. |

## Exertion

Exertion runs from 0 to 1. Its target is the square of horizontal speed as a fraction of 320, so a sustained walk at 160 settles near 0.25 and a sustained run reaches 1. Each jump adds 7% of the remaining headroom. Horizontal speed counts on foot, in water and in the air, so bunny hopping reads as running and a long fall reads as rest.

Breathing follows exertion through a further one-second lag, which also spreads each jump's step out smoothly. It goes from 13 breaths a minute at rest to 45 when spent, and from 0.08 to 0.40 units of rise (0.05 to 0.28 degrees of pitch). The inhale takes about 40% of each breath. Exertion also adds up to 30% to the gait rotation.

## Gait

The stride phase advances with speed and stops while the feet are off the ground, so it never jumps. Footfalls land at phase 0 and pi.

- Twice per stride: the vertical bob and the head nod. The bob has a sharp dip at impact and a rounded top in flight, 2 units down and 1.2 up at run speed. The nod trails the impact slightly.
- Once per stride: the lean and weight shift toward the stance foot, with the shoulder turn a quarter stride ahead of them. Together these trace a figure of eight.

Gait amplitude follows a smoothed speed and a ground-contact blend, so starts, stops, take-offs and landings all fade over a tenth of a second or so. There are no footfalls while airborne, in noclip or at least waist-deep in water.

The weapon inherits the camera's pitch, yaw and most of its roll, then trails each by 30%, and trails the bob and weight shift, which is what gives it weight on screen.

## When it stands down

Everything fades out over about 0.2 seconds in chase view or when the view belongs to another entity. Rotation is dropped at once on death, because the dead view angle is fixed. Intermission and the map editor do not run this code at all. The state resets on each new map.

All timing comes from `cl.time`, and every filter is an exact exponential, so the result does not depend on frame rate. A single frame integrates at most 0.1 seconds. `tests/view_motion_test.c` covers the maths.
