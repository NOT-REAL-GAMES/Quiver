#include "view_motion.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RUN	 320.0f
#define PI_F 3.14159265f

static view_motion_params_t Defaults (void)
{
	view_motion_params_t params = {
		.bob = 0.02f,
		.bob_cycle = 0.6f,
		.bob_up = 0.5f,
		.roll_angle = 2.0f,
		.breath = 1.0f,
		.gait = 1.0f,
		.gait_pitch = 0.25f,
		.gait_yaw = 0.3f,
		.gait_roll = 0.35f,
		.gait_sway = 0.35f,
		.exertion_rise = 10.0f,
		.exertion_recover = 7.0f,
	};
	return params;
}

static view_motion_input_t Moving (float speed)
{
	view_motion_input_t input = {.dt = 0.0f, .speed = speed, .on_ground = true, .active = true};
	return input;
}

// Steps for the given time at a fixed rate, with the last step taking the remainder.
static void Run (view_motion_state_t *state, const view_motion_params_t *params, view_motion_input_t input, float seconds, float dt)
{
	for (float left = seconds; left > 1e-6f; left -= dt)
	{
		input.dt = left < dt ? left : dt;
		ViewMotion_Step (state, params, &input);
	}
}

static float PoseSum (const view_motion_pose_t *pose)
{
	return fabsf (pose->bob) + fabsf (pose->view_up) + fabsf (pose->view_right) + fabsf (pose->view_pitch) + fabsf (pose->view_yaw) + fabsf (pose->view_roll) +
		   fabsf (pose->gun_up) + fabsf (pose->gun_right) + fabsf (pose->gun_pitch) + fabsf (pose->gun_yaw) + fabsf (pose->gun_roll);
}

static float PhaseDifference (float a, float b)
{
	float difference = fabsf (a - b);
	return difference > PI_F ? 2.0f * PI_F - difference : difference;
}

static void TestExertionBuildsAndRecovers (void)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);

	float previous = 0.0f;
	for (int second = 0; second < 40; ++second)
	{
		Run (&state, &params, Moving (RUN), 1.0f, 1.0f / 120.0f);
		assert (state.exertion > previous && state.exertion <= 1.0f);
		previous = state.exertion;
	}
	assert (state.exertion > 0.95f);

	// a sustained walk settles well below a sustained run
	view_motion_state_t walk;
	ViewMotion_Reset (&walk);
	Run (&walk, &params, Moving (160.0f), 60.0f, 1.0f / 120.0f);
	assert (walk.exertion > 0.2f && walk.exertion < 0.3f);

	// rest recovers smoothly: no frame takes more than its share of the exponential
	const float			start = state.exertion;
	view_motion_input_t rest = Moving (0.0f);
	rest.dt = 1.0f / 120.0f;
	float worst_change = 0.0f;
	for (int frame = 0; frame < 120 * 30; ++frame)
	{
		const float before = state.breath_drive;
		ViewMotion_Step (&state, &params, &rest);
		worst_change = fmaxf (worst_change, fabsf (state.breath_drive - before));
	}
	assert (worst_change < 0.01f);
	assert (state.exertion < start * 0.02f);
}

static void TestJumpsAreExertionEvents (void)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (0.0f), 0.1f, 0.01f);

	view_motion_input_t jump = Moving (0.0f);
	jump.on_ground = false;
	jump.vertical_speed = 270.0f;
	Run (&state, &params, jump, 0.01f, 0.01f);
	const float after_takeoff = state.exertion;
	assert (after_takeoff > 0.05f);

	// the rest of the jump costs nothing further
	Run (&state, &params, jump, 0.2f, 0.01f);
	assert (state.exertion < after_takeoff);

	// stepping off a ledge is not a jump
	view_motion_state_t ledge;
	ViewMotion_Reset (&ledge);
	Run (&ledge, &params, Moving (0.0f), 0.1f, 0.01f);
	view_motion_input_t fall = Moving (0.0f);
	fall.on_ground = false;
	fall.vertical_speed = -40.0f;
	Run (&ledge, &params, fall, 0.2f, 0.01f);
	assert (ledge.exertion == 0.0f);

	// the jump's step in exertion reaches the view through the breath lag
	view_motion_state_t lagged;
	ViewMotion_Reset (&lagged);
	Run (&lagged, &params, Moving (0.0f), 0.1f, 0.01f);
	const float drive_before = lagged.breath_drive;
	Run (&lagged, &params, jump, 0.01f, 0.01f);
	assert (lagged.breath_drive - drive_before < 0.002f);
}

static void TestBreathingRateAndDepthRiseWithExertion (void)
{
	const view_motion_params_t params = Defaults ();
	assert (ViewMotion_BreathRate (1.0f) > 3.0f * ViewMotion_BreathRate (0.0f));
	assert (ViewMotion_BreathRate (0.5f) > ViewMotion_BreathRate (0.25f));

	// always present at rest, and subtle
	float				peak_rest = 0.0f, peak_pitch_rest = 0.0f;
	view_motion_state_t state;
	ViewMotion_Reset (&state);
	view_motion_input_t rest = Moving (0.0f);
	rest.dt = 0.01f;
	for (int frame = 0; frame < 1000; ++frame)
	{
		view_motion_pose_t pose;
		ViewMotion_Step (&state, &params, &rest);
		ViewMotion_Pose (&state, &params, &pose);
		peak_rest = fmaxf (peak_rest, fabsf (pose.view_up));
		peak_pitch_rest = fmaxf (peak_pitch_rest, fabsf (pose.view_pitch));
		assert (pose.bob == 0.0f && pose.view_right == 0.0f && pose.view_yaw == 0.0f);
	}
	assert (peak_rest > 0.05f && peak_rest < 0.15f);
	assert (peak_pitch_rest > 0.03f && peak_pitch_rest < 0.1f);

	// spent and standing: the same breath, faster and deeper
	state.exertion = state.breath_drive = 1.0f;
	float peak_spent = 0.0f;
	int	  crossings = 0;
	float previous = 0.0f;
	rest.dt = 0.005f;
	view_motion_params_t hold = params;
	hold.exertion_recover = 600.0f;
	for (int frame = 0; frame < 2000; ++frame)
	{
		view_motion_pose_t pose;
		ViewMotion_Step (&state, &hold, &rest);
		ViewMotion_Pose (&state, &hold, &pose);
		peak_spent = fmaxf (peak_spent, fabsf (pose.view_up));
		if (previous < 0.0f && pose.view_up >= 0.0f)
			crossings += 1;
		previous = pose.view_up;
	}
	assert (peak_spent > 3.0f * peak_rest && peak_spent < 0.5f);
	assert (crossings >= 6 && crossings <= 8); // about 0.75 breaths a second for 10 seconds
}

static void TestGaitFollowsFootfalls (void)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (RUN), 2.0f, 0.002f);

	// with breathing off, count one stride's worth of each channel
	view_motion_params_t gait_only = params;
	gait_only.breath = 0.0f;
	const float step_rate = ViewMotion_StepRate (RUN, params.bob_cycle);
	assert (step_rate > 2.0f && step_rate < 2.7f);
	assert (ViewMotion_StepRate (160.0f, params.bob_cycle) < step_rate);

	view_motion_input_t input = Moving (RUN);
	input.dt = 0.0005f;
	const int		   frames = (int)(10.0f * (2.0f / step_rate) / input.dt);
	view_motion_pose_t previous;
	ViewMotion_Pose (&state, &gait_only, &previous);
	int	  roll_cycles = 0, yaw_cycles = 0, pitch_cycles = 0, bob_cycles = 0;
	float peak_pitch = 0, peak_yaw = 0, peak_roll = 0, peak_right = 0, bob_top = -10, bob_bottom = 10;
	for (int frame = 0; frame < frames; ++frame)
	{
		view_motion_pose_t pose;
		ViewMotion_Step (&state, &gait_only, &input);
		ViewMotion_Pose (&state, &gait_only, &pose);
		roll_cycles += previous.view_roll < 0.0f && pose.view_roll >= 0.0f;
		yaw_cycles += previous.view_yaw < 0.0f && pose.view_yaw >= 0.0f;
		pitch_cycles += previous.view_pitch < 0.0f && pose.view_pitch >= 0.0f;
		bob_cycles += previous.bob < 0.0f && pose.bob >= 0.0f;
		peak_pitch = fmaxf (peak_pitch, fabsf (pose.view_pitch));
		peak_yaw = fmaxf (peak_yaw, fabsf (pose.view_yaw));
		peak_roll = fmaxf (peak_roll, fabsf (pose.view_roll));
		peak_right = fmaxf (peak_right, fabsf (pose.view_right));
		bob_top = fmaxf (bob_top, pose.bob);
		bob_bottom = fminf (bob_bottom, pose.bob);
		previous = pose;
	}

	// ten strides: side-to-side motion once per stride, up-and-down once per footfall
	assert (roll_cycles >= 9 && roll_cycles <= 11);
	assert (yaw_cycles >= 9 && yaw_cycles <= 11);
	assert (pitch_cycles >= 19 && pitch_cycles <= 21);
	assert (bob_cycles >= 19 && bob_cycles <= 21);

	// the cvars are peak values at run speed, before the small exertion gain
	const float gain = 1.0f + 0.3f * state.breath_drive;
	assert (fabsf (peak_pitch - 0.25f * gain) < 0.02f);
	assert (fabsf (peak_yaw - 0.30f * gain) < 0.02f);
	assert (fabsf (peak_roll - 0.35f * gain) < 0.02f);
	assert (fabsf (peak_right - 0.35f * gain) < 0.02f);

	// the impact is deeper than the flight is high
	assert (bob_bottom < -1.9f && bob_top > 1.1f && bob_top < 1.3f);

	// bob_cycle stays in charge of cadence and bob_up of the bob's shape
	assert (ViewMotion_StepRate (RUN, 0.3f) > 1.9f * step_rate);
	assert (ViewMotion_StepRate (RUN, 0.0f) == 0.0f);
}

static void TestGaitScalesWithSpeed (void)
{
	const view_motion_params_t params = Defaults ();
	float					   peaks[2];
	const float				   speeds[2] = {160.0f, RUN};
	for (int i = 0; i < 2; ++i)
	{
		view_motion_state_t state;
		ViewMotion_Reset (&state);
		Run (&state, &params, Moving (speeds[i]), 2.0f, 0.005f);
		state.exertion = state.breath_drive = 0.0f;
		peaks[i] = 0.0f;
		for (int phase = 0; phase < 64; ++phase)
		{
			view_motion_pose_t pose;
			state.gait_phase = phase * 2.0f * PI_F / 64.0f;
			ViewMotion_Pose (&state, &params, &pose);
			peaks[i] = fmaxf (peaks[i], fabsf (pose.view_yaw));
		}
	}
	assert (fabsf (peaks[0] * 2.0f - peaks[1]) < 0.01f);
}

// The worst frame-to-frame jump in any pose channel over a scripted run.
static float WorstJump (float dt)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);

	const view_motion_input_t standing = Moving (0.0f), running = Moving (RUN);
	view_motion_input_t		  rising = running, falling = running, dead = Moving (0.0f);
	rising.on_ground = false;
	rising.vertical_speed = 270.0f;
	falling.on_ground = false;
	falling.vertical_speed = -270.0f;
	dead.active = false;
	struct
	{
		view_motion_input_t input;
		float				seconds;
	} script[] = {
		{standing, 1.0f}, {running, 1.3f}, {rising, 0.35f},	 {falling, 0.35f}, {running, 0.9f},
		{standing, 1.1f}, {running, 0.2f}, {standing, 0.2f}, {running, 1.0f},  {dead, 1.0f},
	};

	view_motion_pose_t previous;
	ViewMotion_Pose (&state, &params, &previous);
	float worst = 0.0f;
	for (size_t i = 0; i < sizeof (script) / sizeof (script[0]); ++i)
		for (float left = script[i].seconds; left > 1e-6f; left -= dt)
		{
			view_motion_pose_t pose;
			script[i].input.dt = dt;
			ViewMotion_Step (&state, &params, &script[i].input);
			ViewMotion_Pose (&state, &params, &pose);
			const float channels[][2] = {
				{pose.bob, previous.bob},
				{pose.view_up, previous.view_up},
				{pose.view_right, previous.view_right},
				{pose.view_pitch, previous.view_pitch},
				{pose.view_yaw, previous.view_yaw},
				{pose.view_roll, previous.view_roll},
				{pose.gun_up, previous.gun_up},
				{pose.gun_right, previous.gun_right},
				{pose.gun_roll, previous.gun_roll},
			};
			for (size_t channel = 0; channel < sizeof (channels) / sizeof (channels[0]); ++channel)
				worst = fmaxf (worst, fabsf (channels[channel][0] - channels[channel][1]));
			previous = pose;
		}
	return worst;
}

static void TestTransitionsAreSmooth (void)
{
	// the largest channel moves about 3 units per footfall, so a continuous pose
	// shrinks in step with the frame time; a snap would not
	const float at_250 = WorstJump (1.0f / 250.0f);
	const float at_1000 = WorstJump (1.0f / 1000.0f);
	assert (at_250 < 0.25f);
	assert (at_1000 < at_250 * 0.4f);

	// leaving the ground fades the stride within a few tenths of a second and landing restores it
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (RUN), 1.0f, 0.005f);
	assert (state.ground > 0.99f);
	view_motion_input_t air = Moving (RUN);
	air.on_ground = false;
	Run (&state, &params, air, 0.4f, 0.005f);
	assert (state.ground < 0.01f);
	const float held_phase = state.gait_phase;
	Run (&state, &params, air, 0.4f, 0.005f);
	assert (PhaseDifference (held_phase, state.gait_phase) < 0.02f);

	// swimming has no footfalls
	view_motion_input_t swim = Moving (RUN);
	swim.in_water = true;
	Run (&state, &params, swim, 1.0f, 0.005f);
	assert (state.ground < 0.001f);

	// the stop settles completely, leaving only breathing
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (RUN), 1.0f, 0.005f);
	Run (&state, &params, Moving (0.0f), 2.0f, 0.005f);
	view_motion_pose_t pose;
	ViewMotion_Pose (&state, &params, &pose);
	assert (fabsf (pose.bob) < 0.001f && fabsf (pose.view_right) < 0.001f);
}

static void TestFrameRateIndependence (void)
{
	const view_motion_params_t params = Defaults ();
	const float				   rates[] = {30.0f, 60.0f, 144.0f, 250.0f, 1000.0f};
	view_motion_state_t		   results[5];
	for (int i = 0; i < 5; ++i)
	{
		view_motion_state_t state;
		ViewMotion_Reset (&state);
		Run (&state, &params, Moving (RUN), 6.0f, 1.0f / rates[i]);
		Run (&state, &params, Moving (0.0f), 3.0f, 1.0f / rates[i]);
		Run (&state, &params, Moving (160.0f), 2.5f, 1.0f / rates[i]);
		results[i] = state;
	}
	for (int i = 0; i < 4; ++i)
	{
		assert (fabsf (results[i].exertion - results[4].exertion) < 0.005f);
		assert (fabsf (results[i].breath_drive - results[4].breath_drive) < 0.005f);
		assert (fabsf (results[i].speed - results[4].speed) < 0.5f);
		assert (PhaseDifference (results[i].gait_phase, results[4].gait_phase) < 0.03f);
		assert (PhaseDifference (results[i].breath_phase, results[4].breath_phase) < 0.03f);
	}

	// uneven frames land in the same place as even ones
	view_motion_state_t uneven;
	ViewMotion_Reset (&uneven);
	const float pattern[] = {0.004f, 0.021f, 0.009f, 0.033f, 0.002f, 0.016f};
	float		elapsed = 0.0f;
	for (int frame = 0; elapsed < 6.0f; ++frame)
	{
		view_motion_input_t input = Moving (RUN);
		input.dt = fminf (pattern[frame % 6], 6.0f - elapsed);
		elapsed += input.dt;
		ViewMotion_Step (&uneven, &params, &input);
		if (input.dt < 0.002f)
			break;
	}
	view_motion_state_t even;
	ViewMotion_Reset (&even);
	Run (&even, &params, Moving (RUN), 6.0f, 1.0f / 1000.0f);
	assert (fabsf (uneven.exertion - even.exertion) < 0.005f);
	assert (PhaseDifference (uneven.gait_phase, even.gait_phase) < 0.03f);
}

static void TestComfortControlsGiveAStillView (void)
{
	// every state a player can reach, including a full sprint on an exhausted body
	view_motion_state_t state;
	ViewMotion_Reset (&state);
	const view_motion_params_t defaults = Defaults ();
	Run (&state, &defaults, Moving (RUN), 30.0f, 0.01f);

	view_motion_input_t input = Moving (RUN);
	input.dt = 0.0037f;
	for (int frame = 0; frame < 2000; ++frame)
	{
		ViewMotion_Step (&state, &defaults, &input);
		view_motion_pose_t pose;

		// cl_bob 0 stills everything, breathing included, with or without cl_rollangle
		view_motion_params_t still = defaults;
		still.bob = 0.0f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (PoseSum (&pose) == 0.0f);
		still.bob = -0.5f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (PoseSum (&pose) == 0.0f);

		// cl_rollangle 0 removes all roll, and only roll
		still = defaults;
		still.roll_angle = 0.0f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (pose.view_roll == 0.0f && pose.gun_roll == 0.0f);
		assert (fabsf (pose.view_yaw) + fabsf (pose.bob) > 0.0f);

		// v_breath 0 and v_gait 0 each remove their own part
		still = defaults;
		still.breath = 0.0f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (pose.view_up == 0.0f);
		still = defaults;
		still.gait = 0.0f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (pose.view_yaw == 0.0f && pose.view_right == 0.0f);

		// cl_bobcycle 0 is V_CalcBob's "don't bob"
		still = defaults;
		still.bob_cycle = 0.0f;
		ViewMotion_Pose (&state, &still, &pose);
		assert (pose.bob == 0.0f && pose.view_yaw == 0.0f);
	}

	// half the bob is half the motion, and the slider's far end stays bounded
	view_motion_params_t scaled = defaults;
	view_motion_pose_t	 full, half;
	ViewMotion_Pose (&state, &defaults, &full);
	scaled.bob = 0.01f;
	ViewMotion_Pose (&state, &scaled, &half);
	assert (fabsf (half.view_yaw * 2.0f - full.view_yaw) < 1e-5f);
	scaled.bob = 1000.0f;
	ViewMotion_Pose (&state, &scaled, &half);
	assert (half.bob >= -7.0f && half.bob <= 4.0f);
	assert (fabsf (half.view_yaw) <= fabsf (full.view_yaw) * 2.5f + 1e-5f);
}

static void TestInactiveFadesOut (void)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state;
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (RUN), 5.0f, 0.005f);

	view_motion_input_t dead = Moving (RUN);
	dead.active = false;
	Run (&state, &params, dead, 3.0f, 0.005f);
	view_motion_pose_t pose;
	ViewMotion_Pose (&state, &params, &pose);
	assert (PoseSum (&pose) < 1e-3f);

	view_motion_input_t rejump = Moving (0.0f);
	rejump.active = false;
	rejump.on_ground = false;
	rejump.vertical_speed = 270.0f;
	const float before = state.exertion;
	Run (&state, &params, Moving (0.0f), 0.01f, 0.01f);
	Run (&state, &params, rejump, 0.01f, 0.01f);
	assert (state.exertion <= before);
}

static void TestBadInput (void)
{
	const view_motion_params_t params = Defaults ();
	view_motion_state_t		   state, reference;
	ViewMotion_Reset (&state);
	Run (&state, &params, Moving (RUN), 1.0f, 0.01f);
	reference = state;

	// no time passed: a paused client or cl.time stepping back must not advance anything
	const float stalled[] = {0.0f, -0.25f, NAN};
	for (int i = 0; i < 3; ++i)
	{
		view_motion_input_t input = Moving (RUN);
		input.dt = stalled[i];
		ViewMotion_Step (&state, &params, &input);
		assert (!memcmp (&state, &reference, sizeof (state)));
	}

	// a hitch integrates no more than one long frame's worth
	view_motion_input_t hitch = Moving (0.0f);
	hitch.dt = 30.0f;
	const float exertion_before = state.exertion;
	ViewMotion_Step (&state, &params, &hitch);
	assert (state.exertion > exertion_before * 0.9f);
	assert (PhaseDifference (state.gait_phase, reference.gait_phase) < 1.0f);

	// a cvar set to nonsense neither breaks the state nor reaches the pose
	view_motion_params_t silly = params;
	silly.exertion_rise = 0.0f;
	silly.exertion_recover = -1.0f;
	silly.bob_up = 40.0f;
	silly.gait_sway = NAN;
	silly.breath = INFINITY;
	silly.gait = -3.0f;
	view_motion_input_t input = Moving (INFINITY);
	input.dt = 0.01f;
	for (int frame = 0; frame < 500; ++frame)
	{
		view_motion_pose_t pose;
		ViewMotion_Step (&state, &silly, &input);
		ViewMotion_Pose (&state, &silly, &pose);
		assert (isfinite (PoseSum (&pose)));
		assert (state.exertion >= 0.0f && state.exertion <= 1.0f);
	}

	ViewMotion_Reset (&state);
	assert (state.exertion == 0.0f && state.presence == 1.0f && state.gait_phase == 0.0f);
}

int main (void)
{
	TestExertionBuildsAndRecovers ();
	TestJumpsAreExertionEvents ();
	TestBreathingRateAndDepthRiseWithExertion ();
	TestGaitFollowsFootfalls ();
	TestGaitScalesWithSpeed ();
	TestTransitionsAreSmooth ();
	TestFrameRateIndependence ();
	TestComfortControlsGiveAStillView ();
	TestInactiveFadesOut ();
	TestBadInput ();
	puts ("view motion tests passed");
	return 0;
}
