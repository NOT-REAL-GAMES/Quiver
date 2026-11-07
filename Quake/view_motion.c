/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
// view_motion.c -- first-person breathing and gait motion. See docs/view-motion.md.

#include "view_motion.h"

#include <math.h>
#include <string.h>

#define VM_PI  3.14159265358979323846f
#define VM_TAU (2.0f * VM_PI)

#define VM_MAX_STEP			0.1f // longest time one step integrates, so a hitch cannot leap
#define VM_MAX_RUN_FRACTION 1.25f

// time constants, seconds
#define VM_SPEED_TAU	 0.12f
#define VM_PLANT_TAU	 0.10f
#define VM_LIFT_TAU		 0.07f
#define VM_PRESENCE_TAU	 0.20f
#define VM_BREATH_LAG	 1.00f
#define VM_MIN_EXERT_TAU 0.05f
#define VM_MAX_EXERT_TAU 600.0f

#define VM_JUMP_SPEED 120.0f // upward speed that separates a jump from stepping off a ledge
#define VM_JUMP_COST  0.07f	 // share of the remaining exertion headroom one jump uses

// Quake's defaults are the 1x reference for the shared comfort controls.
#define VM_DEFAULT_BOB	0.02f
#define VM_DEFAULT_ROLL 2.0f
#define VM_MAX_SCALE	2.5f

#define VM_BOB_UNITS	 2.0f // footfall dip at run speed and default cl_bob
#define VM_BOB_MAX		 4.0f // V_CalcBob's limits
#define VM_BOB_MIN		 -7.0f
#define VM_NOD_LAG		 0.35f // radians of step phase the head nod trails the impact by
#define VM_EXERTION_GAIN 0.3f  // extra gait amplitude when spent

#define VM_MIN_STEP_PERIOD 0.1f
#define VM_CADENCE_BASE	   0.72f // footfalls per cl_bobcycle at a standstill
#define VM_CADENCE_RUN	   0.68f // additional footfalls per cl_bobcycle at run speed

#define VM_BREATH_RATE_REST	  0.22f // breaths per second
#define VM_BREATH_RATE_SPENT  0.75f
#define VM_BREATH_SKEW		  0.35f // shortens the inhale relative to the exhale
#define VM_BREATH_UP_REST	  0.08f // units
#define VM_BREATH_UP_SPENT	  0.40f
#define VM_BREATH_PITCH_REST  0.05f // degrees
#define VM_BREATH_PITCH_SPENT 0.28f
#define VM_BREATH_ROLL_REST	  0.015f
#define VM_BREATH_ROLL_SPENT  0.08f

// how the carried weapon trails the body
#define VM_GUN_BREATH	 0.8f
#define VM_GUN_BOB_LAG	 0.2f
#define VM_GUN_SWAY_LAG	 0.4f
#define VM_GUN_ANGLE_LAG 0.3f

// Written so that NaN, which a cvar can hold, lands on the low bound.
static float ViewMotion_Clamp (float value, float low, float high)
{
	if (!(value > low))
		return low;
	return value < high ? value : high;
}

static float ViewMotion_Lerp (float from, float to, float fraction)
{
	return from + (to - from) * fraction;
}

// Exact first-order response, so the result depends on elapsed time and not on
// how that time was split into frames.
static void ViewMotion_Approach (float *value, float target, float time_constant, float dt)
{
	*value += (target - *value) * (1.0f - expf (-dt / time_constant));
}

static float ViewMotion_WrapPhase (float phase)
{
	phase = fmodf (phase, VM_TAU);
	return phase < 0.0f ? phase + VM_TAU : phase;
}

static float ViewMotion_RunFraction (float speed)
{
	return ViewMotion_Clamp (speed / VIEW_MOTION_RUN_SPEED, 0.0f, VM_MAX_RUN_FRACTION);
}

float ViewMotion_BreathRate (float breath_drive)
{
	return ViewMotion_Lerp (VM_BREATH_RATE_REST, VM_BREATH_RATE_SPENT, ViewMotion_Clamp (breath_drive, 0.0f, 1.0f));
}

float ViewMotion_StepRate (float speed, float bob_cycle)
{
	if (!(bob_cycle > 0.0f))
		return 0.0f;
	return (VM_CADENCE_BASE + VM_CADENCE_RUN * ViewMotion_RunFraction (speed)) / ViewMotion_Clamp (bob_cycle, VM_MIN_STEP_PERIOD, 1e6f);
}

// One footfall per turn of phase, with the foot landing at phase 0. The second
// harmonic sharpens the impact at the bottom and rounds the flight at the top,
// which is what separates a stride from a sine wave. Bottoms out at -1 and peaks
// at 0.6. bob_up moves the peak the same way cl_bobup moves V_CalcBob's.
static float ViewMotion_Footfall (float phase, float bob_up)
{
	const float up = ViewMotion_Clamp (bob_up, 0.1f, 0.9f);
	const float cycle = ViewMotion_WrapPhase (phase) / VM_TAU;
	const float warped = (cycle < up) ? VM_PI * cycle / up : VM_PI + VM_PI * (cycle - up) / (1.0f - up);
	return (-cosf (warped) - 0.25f * cosf (2.0f * warped)) / 1.25f;
}

// A skewed sine: inhaling takes about 40% of the breath, exhaling the rest.
static float ViewMotion_Breath (float phase)
{
	return sinf (phase + VM_BREATH_SKEW * sinf (phase));
}

void ViewMotion_Reset (view_motion_state_t *state)
{
	memset (state, 0, sizeof (*state));
	state->presence = 1.0f;
}

void ViewMotion_Step (view_motion_state_t *state, const view_motion_params_t *params, const view_motion_input_t *input)
{
	if (!(input->dt > 0.0f))
		return;
	const float dt = input->dt < VM_MAX_STEP ? input->dt : VM_MAX_STEP;

	// phases integrate by trapezoid so that accelerating is frame-rate independent too
	const float old_step_speed = VM_PI * ViewMotion_StepRate (state->speed, params->bob_cycle) * state->ground;
	const float old_breath_speed = VM_TAU * ViewMotion_BreathRate (state->breath_drive);

	const bool	planted = input->active && input->on_ground && !input->in_water;
	const float speed = input->active ? ViewMotion_Clamp (input->speed, 0.0f, 1e6f) : 0.0f;
	ViewMotion_Approach (&state->speed, speed, VM_SPEED_TAU, dt);
	ViewMotion_Approach (&state->ground, planted ? 1.0f : 0.0f, planted ? VM_PLANT_TAU : VM_LIFT_TAU, dt);
	ViewMotion_Approach (&state->presence, input->active ? 1.0f : 0.0f, VM_PRESENCE_TAU, dt);

	// horizontal speed drives exertion on foot, in water and in the air alike, so
	// a long fall reads as rest while bunny hopping still reads as running
	const float run = ViewMotion_Clamp (state->speed / VIEW_MOTION_RUN_SPEED, 0.0f, 1.0f);
	const float target = run * run;
	const float exertion_time = (target > state->exertion) ? params->exertion_rise : params->exertion_recover;
	ViewMotion_Approach (&state->exertion, target, ViewMotion_Clamp (exertion_time, VM_MIN_EXERT_TAU, VM_MAX_EXERT_TAU), dt);

	if (input->active && state->was_on_ground && !input->on_ground && !input->in_water && input->vertical_speed >= VM_JUMP_SPEED)
		state->exertion += VM_JUMP_COST * (1.0f - state->exertion);
	state->was_on_ground = input->on_ground;

	// the jump above is a step in exertion; nothing visible reads exertion directly
	ViewMotion_Approach (&state->breath_drive, state->exertion, VM_BREATH_LAG, dt);

	const float new_step_speed = VM_PI * ViewMotion_StepRate (state->speed, params->bob_cycle) * state->ground;
	const float new_breath_speed = VM_TAU * ViewMotion_BreathRate (state->breath_drive);
	state->gait_phase = ViewMotion_WrapPhase (state->gait_phase + 0.5f * (old_step_speed + new_step_speed) * dt);
	state->breath_phase = ViewMotion_WrapPhase (state->breath_phase + 0.5f * (old_breath_speed + new_breath_speed) * dt);
}

void ViewMotion_Pose (const view_motion_state_t *state, const view_motion_params_t *params, view_motion_pose_t *pose)
{
	memset (pose, 0, sizeof (*pose));

	// cl_bob and cl_rollangle stay the master controls, so settings that give a
	// still view today keep giving a still view
	const float master = ViewMotion_Clamp (params->bob / VM_DEFAULT_BOB, 0.0f, VM_MAX_SCALE) * state->presence;
	const float roll_master = ViewMotion_Clamp (params->roll_angle / VM_DEFAULT_ROLL, 0.0f, VM_MAX_SCALE);
	if (!(master > 0.0f))
		return;

	const float drive = ViewMotion_Clamp (state->breath_drive, 0.0f, 1.0f);
	const float breath = master * ViewMotion_Clamp (params->breath, 0.0f, 3.0f);
	const float breath_wave = ViewMotion_Breath (state->breath_phase);
	const float breath_up = breath * ViewMotion_Lerp (VM_BREATH_UP_REST, VM_BREATH_UP_SPENT, drive) * breath_wave;
	const float breath_pitch = -breath * ViewMotion_Lerp (VM_BREATH_PITCH_REST, VM_BREATH_PITCH_SPENT, drive) * breath_wave;
	const float breath_roll = breath * roll_master * ViewMotion_Lerp (VM_BREATH_ROLL_REST, VM_BREATH_ROLL_SPENT, drive) * cosf (state->breath_phase);

	// footfalls land at phase 0 (right) and pi (left)
	const float stride = (params->bob_cycle > 0.0f) ? state->ground * ViewMotion_RunFraction (state->speed) : 0.0f;
	const float step_phase = 2.0f * state->gait_phase;
	const float body = master * ViewMotion_Clamp (params->gait, 0.0f, 3.0f) * stride * (1.0f + VM_EXERTION_GAIN * drive);
	const float across = sinf (state->gait_phase);
	const float nod = -ViewMotion_Footfall (step_phase - VM_NOD_LAG, params->bob_up);

	pose->bob = ViewMotion_Clamp (master * stride * VM_BOB_UNITS * ViewMotion_Footfall (step_phase, params->bob_up), VM_BOB_MIN, VM_BOB_MAX);
	pose->view_up = breath_up;
	// the weight settles over the stance foot between footfalls, while the shoulders
	// turn against the leading leg, peaking a quarter stride earlier: a figure of eight
	pose->view_right = body * ViewMotion_Clamp (params->gait_sway, 0.0f, 8.0f) * across;
	pose->view_pitch = body * params->gait_pitch * nod + breath_pitch;
	pose->view_yaw = -body * params->gait_yaw * cosf (state->gait_phase);
	pose->view_roll = body * roll_master * params->gait_roll * across + breath_roll;

	pose->gun_up = VM_GUN_BREATH * breath_up - VM_GUN_BOB_LAG * pose->bob;
	pose->gun_right = -VM_GUN_SWAY_LAG * pose->view_right;
	pose->gun_pitch = -VM_GUN_ANGLE_LAG * pose->view_pitch;
	pose->gun_yaw = -VM_GUN_ANGLE_LAG * pose->view_yaw;
	pose->gun_roll = (1.0f - VM_GUN_ANGLE_LAG) * pose->view_roll;
}
