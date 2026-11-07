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
// view_motion.h -- first-person breathing and gait motion. Pure maths with no
// engine dependencies; view.c feeds it client state and applies the pose to the
// rendered camera and weapon only.

#ifndef QUIVER_VIEW_MOTION_H
#define QUIVER_VIEW_MOTION_H

#include <stdbool.h>

#define VIEW_MOTION_RUN_SPEED 320.0f

typedef struct view_motion_input_s
{
	float dt;			  // seconds of client time since the previous step
	float speed;		  // horizontal speed, units per second
	float vertical_speed; // units per second, positive is up
	bool  on_ground;
	bool  in_water;
	bool  active; // false while dead, in chase view or watching another entity
} view_motion_input_t;

typedef struct view_motion_params_s
{
	float bob;				// cl_bob: 0 stills all of this motion
	float bob_cycle;		// cl_bobcycle: 0 stills the gait
	float bob_up;			// cl_bobup
	float roll_angle;		// cl_rollangle: 0 removes all of this roll
	float breath;			// v_breath
	float gait;				// v_gait
	float gait_pitch;		// v_gait_pitch, peak degrees at run speed
	float gait_yaw;			// v_gait_yaw, peak degrees at run speed
	float gait_roll;		// v_gait_roll, peak degrees at run speed
	float gait_sway;		// v_gait_sway, peak units at run speed
	float exertion_rise;	// v_exertion_rise, time constant in seconds
	float exertion_recover; // v_exertion_recover, time constant in seconds
} view_motion_params_t;

typedef struct view_motion_state_s
{
	float speed;		// smoothed horizontal speed
	float ground;		// foot contact blend, 0 airborne to 1 planted
	float presence;		// overall blend, falls to 0 while inactive
	float exertion;		// 0 rested to 1 spent
	float breath_drive; // exertion as the lungs feel it, lags behind exertion
	float gait_phase;	// radians, a full turn is one stride of two footfalls
	float breath_phase; // radians, a full turn is one breath
	bool  was_on_ground;
} view_motion_state_t;

// All offsets are relative to the unmoved view. Angles are degrees in view
// convention (positive pitch looks down, positive yaw turns left, positive roll
// leans right). The gun_ fields are added on top of what the weapon inherits
// from the camera, except gun_roll, which the weapon does not otherwise inherit.
typedef struct view_motion_pose_s
{
	float bob;		  // gait height, replaces V_CalcBob
	float view_up;	  // breathing height
	float view_right; // gait weight shift
	float view_pitch;
	float view_yaw;
	float view_roll;
	float gun_up;
	float gun_right;
	float gun_pitch;
	float gun_yaw;
	float gun_roll;
} view_motion_pose_t;

void ViewMotion_Reset (view_motion_state_t *state);
void ViewMotion_Step (view_motion_state_t *state, const view_motion_params_t *params, const view_motion_input_t *input);
void ViewMotion_Pose (const view_motion_state_t *state, const view_motion_params_t *params, view_motion_pose_t *pose);

// Breaths per second, and footfalls per second, for the given state values.
float ViewMotion_BreathRate (float breath_drive);
float ViewMotion_StepRate (float speed, float bob_cycle);

#endif
