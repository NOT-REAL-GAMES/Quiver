/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "jolt_stream_character.h"
#include "jolt_stream_world.h"
#include "world_solids.h"
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace quiver;
namespace
{
void Check (bool value, const char *message)
{
	if (!value)
		throw std::runtime_error (message);
}
WorldPosition At (ChunkKey key, std::array<float, 3> local)
{
	WorldPosition result{key, local};
	Check (Normalize (result), "normalize fixture");
	return result;
}
void Load (World &world, ChunkKey key)
{
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
				Check (world.Require ({key.x + x, key.y + y, key.z + z}), "load fixture neighborhood");
}
JoltEntityProxy Box (uint64_t id, ChunkKey key, std::array<float, 3> lo, std::array<float, 3> hi)
{
	return {id, At (key, {}), lo, hi, {}};
}
struct Driver
{
	JoltStreamCharacter &character;
	JoltCharacterInput	 input;
	JoltCharacterResult	 output;
	explicit Driver (JoltStreamCharacter &c, WorldPosition position) : character (c)
	{
		input.position = position;
		input.deltaTime = 1.0f / 120;
	}
	void Tick (float x = 0, float y = 0)
	{
		input.velocity[0] = x;
		input.velocity[1] = y;
		if (input.grounded && input.velocity[2] < 0)
			input.velocity[2] = 0;
		input.velocity[2] -= 800 * input.deltaTime;
		output = character.Step (input);
		if (output.blockedUnknown)
			throw std::runtime_error (
				"known fixture unexpectedly blocked as unknown at chunk " + std::to_string (input.position.chunk.x) + "," +
				std::to_string (input.position.chunk.y) + "," + std::to_string (input.position.chunk.z) + " local " + std::to_string (input.position.local[0]) +
				"," + std::to_string (input.position.local[1]) + "," + std::to_string (input.position.local[2]));
		Check (!output.hitLimit, "fixture exceeded contact budget");
		input.position = output.position;
		input.velocity = output.velocity;
		input.grounded = output.grounded;
	}
	void Land ()
	{
		for (int i = 0; i < 360; ++i)
		{
			Tick ();
			if (input.grounded && std::abs (input.velocity[2]) < .1f)
				return;
		}
		throw std::runtime_error ("character failed to land");
	}
	float Feet () const
	{
		return input.position.local[2] + input.mins[2];
	}
};
void TerrainTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world, {});
	JoltStreamWorld physics (world);
	auto			trace = physics.Trace (At ({}, {128, 128, 200}), {0, 0, -180});
	Check (!trace.hitUnknown && std::abs (trace.end.local[2] - 64) < .05, "Jolt ray matches exact terrain spawn plane");
	trace = physics.Trace (At ({}, {128, 128, 200}), {0, 0, -180}, {-16, -16, -24}, {16, 16, 32});
	Check (!trace.hitUnknown && std::abs (trace.end.local[2] - 88) < .1, "Jolt box sweep retains Quake hull feet");
	auto builds = physics.Stats ().shapeBuilds;
	for (int i = 0; i < 8; ++i)
		physics.Trace (At ({}, {128, 128, 200}), {0, 0, -180}, {-16, -16, -24}, {16, 16, 32});
	Check (physics.Stats ().shapeBuilds == builds && physics.Stats ().cacheHits > 0, "unchanged queries reuse cooked Jolt shapes");
	auto tangent = physics.Trace (At ({}, {128, 128, 88}), {80, 0, 0}, {-16, -16, -24}, {16, 16, 32});
	Check (!tangent.startSolid && tangent.fraction == 1, "touching floor permits tangent movement");
	auto descending = physics.Trace (At ({}, {112, 128, 88.125f}), {80, 0, -.25}, {-16, -16, -24}, {16, 16, 32});
	Check (
		!descending.startSolid && descending.fraction > .1 && descending.fraction < .9 && descending.normal[2] > .99f,
		"small descending sweep across a tile seam contacts the floor face");

	auto inside = physics.Trace (At ({}, {128, 128, 40}), {0, 0, 0}, {-8, -8, -8}, {8, 8, 8});
	Check (inside.startSolid && inside.allSolid, "stationary embedded hull classified solid");
	auto escape = physics.Trace (At ({}, {128, 128, 40}), {0, 0, 100}, {-8, -8, -8}, {8, 8, 8});
	Check (escape.startSolid && !escape.allSolid, "embedded hull can escape terrain");
	Check (world.Edit (At ({}, {132, 132, 76}), {3, Solid}), "edit natural terrain");
	trace = physics.Trace (At ({}, {132, 132, 120}), {0, 0, -80});
	Check (trace.end.local[2] > 76 && physics.Stats ().shapeBuilds > builds, "edited terrain invalidates cooked collision shape");
	JoltStreamCharacter character (physics, 1);
	Driver				player (character, At ({}, {200, 200, 160}));
	player.Land ();
	Check (std::abs (player.Feet () - 64) < .15, "CharacterVirtual lands on marching-tetra surface");
	for (int i = 0; i < 120; ++i)
		player.Tick (160);
	Check (
		player.input.position.local[0] > 350 && std::abs (player.Feet () - 64) < .15,
		"CharacterVirtual crosses marching-tetra patch seams without a ghost wall");

	auto buriedInput = player.input;
	buriedInput.position = At ({}, {128, 128, -64});
	buriedInput.grounded = false;
	buriedInput.velocity = {160, 0, 0};
	const auto buried = character.Step (buriedInput);
	Check (
		buried.blockedSolid && buried.position.chunk == buriedInput.position.chunk && buried.position.local == buriedInput.position.local,
		"deeply buried character cannot tunnel through missing interior triangles");

	Load (world, {3, -2, 0});
	for (float x : {16.0f, 127.75f, 128.0f, 255.5f, 512.0f, 1023.5f})
		for (float y : {16.0f, 128.0f, 511.0f, 1000.0f})
		{
			const auto start = At ({3, -2, 0}, {x, y, 200});
			const auto reference = world.Trace (start, {0, 0, -180});
			const auto actual = physics.Trace (start, {0, 0, -180});
			Check (
				!actual.hitUnknown && reference.fraction < 1 && actual.fraction < 1 && std::abs (reference.end.local[2] - actual.end.local[2]) < .06,
				"Jolt queries match independent continuous terrain traces across tile/chunk seams");
		}
	const float extreme = std::numeric_limits<float>::max ();
	Check (
		!physics.Sync (At ({}, {128, 128, 100}), {}, {-extreme, -extreme, -extreme}, {extreme, extreme, extreme}),
		"extreme finite hull bounds fail closed before integer conversion");
	const auto excessive = physics.Trace (At ({}, {128, 128, 100}), {}, {-extreme, -extreme, -extreme}, {extreme, extreme, extreme});
	Check (excessive.hitUnknown && excessive.fraction == 0, "extreme trace remains conservatively blocking");
	auto unknown = physics.Trace (At ({500, 500, 0}, {512, 512, 100}), {64, 0, 0});
	Check (unknown.hitUnknown, "unloaded chunks remain blocking");
	JoltCharacterInput missing;
	missing.position = At ({500, 500, 0}, {512, 512, 100});
	missing.deltaTime = 1.0f / 120;
	missing.velocity = {320, 0, 0};
	auto stopped = character.Step (missing);
	Check (
		stopped.blockedUnknown && stopped.position.chunk == missing.position.chunk && stopped.position.local == missing.position.local,
		"controller cannot enter unloaded collision data");
}
void LegacyMovementTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world, {});
	JoltStreamWorld		physics (world);
	JoltStreamCharacter character (physics, 1);
	std::string			failure;
	for (int frequency : {72, 144})
	{
		character.Reset ();
		Driver transition (character, At ({}, {128, 128, 160}));
		transition.input.deltaTime = 1.0f / frequency;
		transition.Land ();
		Check (std::abs (transition.Feet () - 64) < .15, "pristine legacy transition starts on floor64");
		const auto start = transition.input.position;
		int		   supportedFrames = 0;
		for (int i = 0; i < frequency * 3; ++i)
		{
			transition.Tick (320);
			supportedFrames += transition.input.grounded;
		}
		std::array<double, 3> traveled;
		Check (WorldDelta (config, transition.input.position, start, traveled), "legacy movement delta");
		if (traveled[0] <= 900)
			failure += "pristine legacy terrain blocked at " + std::to_string (frequency) + " Hz, x=" + std::to_string (transition.input.position.local[0]) +
					   " z=" + std::to_string (transition.input.position.local[2]) + " distance=" + std::to_string (traveled[0]) + "; ";
		Check (supportedFrames > frequency * 3 / 2, "legacy gentle slopes retain support");
	}
	for (const std::array<float, 3> position : {std::array<float, 3>{371.983154f, 128, 88.632347f}, {371.965851f, 128, 88.631248f}})
		for (int frequency : {72, 144})
		{
			character.Reset ();
			Driver frozen (character, At ({}, position));
			frozen.input.deltaTime = 1.0f / frequency;
			frozen.input.grounded = true;
			frozen.input.velocity = {320, 0, 25.276724f};
			for (int i = 0; i < frequency; ++i)
				frozen.Tick (320);
			if (frozen.input.position.local[0] < 650)
				failure += "exact runtime frozen state " + std::to_string (position[0]) + " at " + std::to_string (frequency) +
						   " Hz stays blocked at x=" + std::to_string (frozen.input.position.local[0]) +
						   " z=" + std::to_string (frozen.input.position.local[2]) + "; ";
		}
	if (!failure.empty ())
		throw std::runtime_error (failure);

	// Contact generation must work throughout an eight-unit terrain cell,
	// rather than only at a favorable approach phase or physics frequency.
	for (int frequency : {72, 120, 144, 200, 0})
		for (int phase = 0; phase < 8; ++phase)
			for (bool accelerated : {false, true})
			{
				character.Reset ();
				Driver approach (character, At ({}, {float (128 + phase), 128, 160}));
				approach.input.deltaTime = 1.0f / (frequency ? frequency : 120);
				approach.Land ();
				const auto start = approach.input.position;
				double	   elapsed = 0;
				int		   frames = 0, supported = 0;
				while (elapsed < 3)
				{
					approach.input.deltaTime = 1.0f / (frequency ? frequency : (frames % 2 ? 144 : 120));
					float velocity = 320;
					if (accelerated)
					{
						// Quake-style ground friction and acceleration, using the
						// velocity returned by the controller on the preceding tick.
						velocity = std::max (0.0f, approach.input.velocity[0]);
						velocity = std::max (0.0f, velocity - std::max (100.0f, velocity) * 6 * approach.input.deltaTime);
						velocity += std::min (320 - velocity, 10 * 320 * approach.input.deltaTime);
					}
					approach.Tick (velocity);
					elapsed += approach.input.deltaTime;
					++frames;
					supported += approach.input.grounded;
				}
				std::array<double, 3> traveled;
				Check (WorldDelta (config, approach.input.position, start, traveled), "terrain approach phase delta");
				if (traveled[0] < 900 || supported * 10 < frames * 9)
					throw std::runtime_error (
						"legacy terrain approach phase " + std::to_string (phase) + " at " + std::to_string (frequency) +
						" Hz (0=jitter), accelerated=" + std::to_string (accelerated) + " traveled=" + std::to_string (traveled[0]) +
						" supported=" + std::to_string (supported) + "/" + std::to_string (frames));
			}
}
void RenderToggleTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	config.smoothTerrain = false;
	World world (config);
	Load (world, {3, -2, 0});
	JoltStreamWorld physics (world);
	bool			fractional = false;
	for (float x : {141.25f, 390.5f, 701.75f})
		for (float y : {183.25f, 512.0f, 900.0f})
		{
			const auto start = At ({3, -2, 0}, {x, y, 200});
			const auto reference = world.Trace (start, {0, 0, -180});
			const auto actual = physics.Trace (start, {0, 0, -180});
			Check (
				!actual.hitUnknown && reference.fraction < 1 && actual.fraction < 1 && std::abs (reference.end.local[2] - actual.end.local[2]) < .06,
				"block-rendering control does not change smooth terrain collision");
			fractional |= std::abs (reference.end.local[2] / 8 - std::round (reference.end.local[2] / 8)) > .1f;
		}
	Check (fractional, "render-toggle regression exercises non-voxel-aligned terrain heights");
}
void ControllerTests ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	World world (config);
	Load (world, {});
	JoltStreamWorld		physics (world);
	JoltStreamCharacter character (physics, 1);
	const auto			floor = Box (100, {}, {-256, -256, 96}, {2048, 2048, 128});
	physics.SetEntityProxies ({floor});
	Driver player (character, At ({}, {256, 256, 240}));
	player.Land ();
	Check (std::abs (player.Feet () - 128) < .15 && player.output.groundUserData == 100, "proxy support preserves floor and entity identity");
	auto swimming = player.input;
	swimming.stickToFloor = false;
	swimming.velocity = {0, 0, 120};
	const auto swimResult = character.Step (swimming);
	Check (
		swimResult.position.local[2] > swimming.position.local[2] + .8f && swimResult.velocity[2] > 100,
		"upward water movement escapes ground when floor sticking is disabled");
	character.Reset ();
	player.Land ();

	for (int i = 0; i < 120; ++i)
		player.Tick (160);
	Check (player.input.position.local[0] > 410 && std::abs (player.Feet () - 128) < .15, "walk on proxy support");
	const float before = player.Feet ();
	player.input.grounded = false;
	player.input.velocity[2] = 270;
	float highest = before;
	for (int i = 0; i < 90; ++i)
	{
		player.Tick ();
		highest = std::max (highest, player.Feet ());
	}
	player.Land ();
	Check (highest - before > 40 && std::abs (player.Feet () - 128) < .15, "jump retains velocity and lands again");

	for (float height : {16.0f, 20.0f})
	{
		physics.SetEntityProxies ({floor, Box (101, {}, {400, 192, 128}, {700, 320, 128 + height})});
		character.Reset ();
		Driver stairs (character, At ({}, {320, 256, 180}));
		stairs.Land ();
		for (int i = 0; i < 120; ++i)
			stairs.Tick (160);
		if (height < 18)
			Check (stairs.input.position.local[0] > 450 && std::abs (stairs.Feet () - 144) < .2, "16-unit stair is walkable");
		else
			Check (stairs.input.position.local[0] < 384.2 && std::abs (stairs.Feet () - 128) < .2, "20-unit ledge exceeds stair limit");
	}
	physics.SetEntityProxies ({floor, Box (102, {}, {400, 192, 128}, {432, 384, 384})});
	character.Reset ();
	Driver wall (character, At ({}, {320, 256, 180}));
	wall.Land ();
	for (int i = 0; i < 120; ++i)
		wall.Tick (160, 40);
	Check (wall.input.position.local[0] < 384.2 && wall.input.position.local[1] > 290, "wall blocks forward motion and allows sliding");

	physics.SetEntityProxies ({floor, Box (103, {}, {192, 192, 208}, {384, 384, 240})});
	character.Reset ();
	Driver ceiling (character, At ({}, {256, 256, 152}));
	ceiling.Land ();
	ceiling.input.grounded = false;
	ceiling.input.velocity[2] = 270;
	highest = ceiling.Feet ();
	for (int i = 0; i < 80; ++i)
	{
		ceiling.Tick ();
		highest = std::max (highest, ceiling.Feet ());
	}
	ceiling.Land ();
	Check (highest > 140 && highest <= 152.2, "jump collides with low ceiling");

	// A real extracted CSG ramp ends at a horizontal top. Keeping the
	// uphill tangent velocity after this crest must not launch the character.
	SolidDefinition ramp, top;
	Check (MakeSolidWedge (At ({}, {384, 512, 192}), {256, 256, 128}, {}, 3, ramp) && world.AddSolid (ramp), "build ridge ramp");
	Check (MakeSolidBox (At ({}, {640, 512, 192}), {256, 256, 128}, {}, 3, top) && world.AddSolid (top), "build ridge top");
	Load (world, {}); // Solid commits replace affected resident snapshots asynchronously.
	physics.SetEntityProxies ({floor});
	character.Reset ();
	Driver ridge (character, At ({}, {160, 512, 152}));
	try
	{
		ridge.Land ();
	}
	catch (const std::exception &error)
	{
		throw std::runtime_error (std::string (error.what ()) + " Jolt: " + physics.LastError () + " world: " + world.LastError ());
	}
	int topFrames = 0, topGrounded = 0;
	for (int i = 0; i < 180; ++i)
	{
		ridge.Tick (320);
		if (ridge.input.position.local[0] > 532)
		{
			++topFrames;
			topGrounded += ridge.input.grounded;
			Check (ridge.Feet () <= 256.3f, "walking over an uphill crest does not launch above the top");
		}
	}
	Check (topFrames > 20 && topGrounded * 10 >= topFrames * 9, "uphill-to-flat transition retains grounded support");

	auto moving = floor;
	moving.velocity = {0, 0, 20};
	physics.SetEntityProxies ({moving});
	character.Reset ();
	Driver platform (character, At ({}, {256, 256, 152}));
	platform.Tick ();
	Check (
		platform.output.grounded && platform.output.groundUserData == 100 && std::abs (platform.output.groundVelocity[2] - 20) < .01,
		"kinematic support exposes platform velocity and identity");

	const ChunkKey huge{9007199254740993LL, -9007199254740993LL, 130};
	Load (world, huge);
	physics.SetEntityProxies ({Box (100, huge, {-256, -256, 96}, {2048, 2048, 128})});
	Driver rebased (character, At (huge, {256, 256, 200}));
	rebased.Land ();
	Check (
		rebased.input.position.chunk == huge && std::abs (rebased.Feet () - 128) < .15 && physics.Stats ().rebases > 0,
		"huge horizontal and high-altitude rebases retain precise support");
	const auto origin = rebased.input.position;
	const auto roundtrip = physics.FromLocal (physics.ToLocal (origin));
	Check (roundtrip.chunk == origin.chunk && roundtrip.local == origin.local, "local-meter conversion preserves canonical position");
	Check (physics.Stats ().shapeBytes <= 32 * 1024 * 1024 && physics.Stats ().shapes <= 4096, "Jolt shape cache remains bounded");
}
void LandingJumpTests ()
{
	for (Generator generator : {Generator::Empty, Generator::Terrain})
	{
		WorldConfig config;
		config.generator = generator;
		World world (config);
		Load (world, {});
		JoltStreamWorld physics (world);
		const float		floor = generator == Generator::Empty ? 128 : 64;
		if (generator == Generator::Empty)
			physics.SetEntityProxies ({Box (100, {}, {-256, -256, 96}, {2048, 2048, 128})});
		for (float rate : {72.f, 120.f, 144.f})
		{
			JoltStreamCharacter character (physics, 1);
			Driver				player (character, At ({}, {128, 128, floor + 512}));
			player.input.deltaTime = 1 / rate;
			for (int jump = 0; jump < 8; ++jump)
			{
				// Stop on the first landing frame: waiting for velocity to settle
				// hides a stale downward request returned by CharacterVirtual.
				int frames = 0;
				while (!player.input.grounded && frames++ < int (rate * 3))
					player.Tick ();
				Check (player.input.grounded, "repeated jump lands within its deadline");
				Check (std::abs (player.input.velocity[2]) < .1f, "first landing frame clears velocity into the support");
				const float previous = player.Feet ();
				// QuakeC adds the jump impulse after observing FL_ONGROUND.
				player.input.grounded = false;
				player.input.velocity[2] += 270;
				player.Tick ();
				Check (
					!player.input.grounded && player.Feet () > previous + 1 && player.input.velocity[2] > 250,
					"jump on the first frame after landing preserves its full impulse");
			}
		}
	}
}

void SlopeLandingJumpTests ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	World			world (config);
	SolidDefinition ramp;
	Check (MakeSolidWedge (At ({}, {384, 512, 192}), {256, 256, 128}, {}, 3, ramp) && world.AddSolid (ramp), "build landing ramp");
	Load (world, {});
	JoltStreamWorld physics (world);
	for (float rate : {72.f, 120.f, 144.f})
	{
		JoltStreamCharacter character (physics, 1);
		Driver				player (character, At ({}, {384, 512, 720}));
		player.input.deltaTime = 1 / rate;
		for (int jump = 0; jump < 4; ++jump)
		{
			int frames = 0;
			while (!player.input.grounded && frames++ < int (rate * 3))
				player.Tick ();
			Check (player.input.grounded && player.output.groundNormal[0] < -.2f, "fall lands on the sloped face");
			Check (
				std::abs (player.input.velocity[0]) < .1f && std::abs (player.input.velocity[1]) < .1f && std::abs (player.input.velocity[2]) < .1f,
				"slope landing removes fall speed without injecting lateral drift");
			const float before = player.Feet ();
			player.input.grounded = false;
			player.input.velocity[2] += 270;
			player.Tick ();
			Check (
				!player.input.grounded && player.Feet () > before + 1 && player.input.velocity[2] > 250,
				"immediate jump after slope landing retains full impulse");
		}
		// Downhill horizontal intent must not become a negative landing Z that
		// consumes QC's additive jump impulse on the next frame.
		character.Reset ();
		Driver downhill (character, At ({}, {448, 512, 320}));
		downhill.input.deltaTime = 1 / rate;
		downhill.input.velocity = {-320, 0, -300};
		int frames = 0;
		while (!downhill.input.grounded && frames++ < int (rate))
			downhill.Tick (-320);
		Check (downhill.input.grounded && downhill.output.groundNormal[0] < -.2f, "downhill arrival lands on the sloped face");
		Check (
			std::abs (downhill.input.velocity[0] + 320) < .1f && std::abs (downhill.input.velocity[1]) < .1f && downhill.input.velocity[2] >= -.1f,
			"downhill landing preserves horizontal intent and removes downward jump cancellation");
		const float before = downhill.Feet ();
		downhill.input.grounded = false;
		downhill.input.velocity[2] += 270;
		downhill.Tick (-320);
		Check (
			!downhill.input.grounded && downhill.Feet () > before + 1 && downhill.input.velocity[2] > 250,
			"immediate downhill landing jump retains full impulse");
	}
}

void AsyncCharacterTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world, {});
	JoltStreamWorld		physics (world, true);
	JoltStreamCharacter character (physics, 1);
	JoltCharacterInput	input;
	input.position = At ({}, {128, 128, 88});
	input.deltaTime = 1.f / 120;
	input.grounded = true;
	input.velocity = {0, 0, -800 * input.deltaTime};
	const auto waiting = character.Step (input);
	Check (
		waiting.pending && waiting.grounded && waiting.velocity == input.velocity && waiting.position.chunk == input.position.chunk &&
			waiting.position.local == input.position.local,
		"cold scene defers without consuming valid support or changing position");
	// QC may accept a jump while a scene is still being prepared. A deferred
	// physics frame must preserve that decision until the scene can simulate it.
	input.grounded = false;
	input.velocity[2] = 270 - 800 * input.deltaTime;
	const auto			deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
	JoltCharacterResult result;
	do
	{
		world.Pump ();
		result = character.Step (input);
		if (result.pending)
		{
			Check (
				!result.grounded && result.velocity == input.velocity && result.position.chunk == input.position.chunk &&
					result.position.local == input.position.local,
				"pending scene retains a queued jump without gravity or contact changes");
			std::this_thread::sleep_for (std::chrono::milliseconds (1));
		}
	} while (result.pending && std::chrono::steady_clock::now () < deadline);
	Check (!result.pending && !result.blockedUnknown, "async collision scene becomes ready within its deadline");
	Check (
		!result.grounded && result.position.local[2] > input.position.local[2] + 1 && result.velocity[2] > 250,
		"queued jump launches with its impulse intact when collision becomes ready");
}

void SolidFloorTests ()
{
	for (Generator generator : {Generator::Empty, Generator::Planet})
	{
		WorldConfig config;
		config.generator = generator;
		config.planetSettings.globalResolution = 32;
		config.planetSettings.extentChunks = 512;
		World			world (config);
		SolidDefinition floor;
		constexpr float floorHeight = 1024.03125f;
		Check (
			MakeSolidBox (At ({1, 0, 0}, {512.03125f, 128.03125f, floorHeight - 32}), {4096, 384, 64}, {}, 3, floor) && world.AddSolid (floor),
			"build extracted floor across vertical chunk boundary");
		Load (world, {0, 0, 1});
		Load (world, {1, 0, 1});
		JoltStreamWorld		physics (world);
		JoltStreamCharacter character (physics, 1);
		for (int frequency : {72, 120, 144, 200})
			for (float y : {128.0f, 128.000153f})
			{
				character.Reset ();
				Driver player (character, At ({}, {128, y, floorHeight + 100}));
				player.input.deltaTime = 1.0f / frequency;
				player.Land ();
				const auto start = player.input.position;
				for (int i = 0; i < frequency * 3; ++i)
				{
					player.Tick (320);
					const double feet = double (player.input.position.chunk.z) * ChunkSize + player.Feet ();
					if (!player.input.grounded || std::abs (feet - floorHeight) > .75)
						throw std::runtime_error (
							"extracted floor support mismatch, generator=" + std::to_string (int (generator)) + " frequency=" + std::to_string (frequency) +
							" startY=" + std::to_string (y) + " x=" + std::to_string (player.input.position.local[0]) + " feet=" + std::to_string (feet));
				}
				std::array<double, 3> traveled;
				Check (WorldDelta (config, player.input.position, start, traveled) && traveled[0] > 900, "walk across extracted floor patches");
			}
	}
}
void CharacterCacheTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world, {});
	Load (world, {1, 0, 0});
	JoltStreamWorld			   physics (world);
	const auto				   first = At ({}, {1023.5f, 128, 120});
	const auto				   second = At ({1, 0, 0}, {.5f, 128, 120});
	const std::array<float, 3> mins{-32, -32, -80}, maxs{32, 32, 60};
	Check (physics.SyncCharacter (first, {}, mins, maxs), "publish welded character scene");
	const auto before = physics.Stats ();
	for (int i = 0; i < 8; ++i)
		Check (physics.SyncCharacter (first, {}, mins, maxs), "warm character scene publication");
	Check (physics.Stats ().shapeBuilds == before.shapeBuilds, "warm character publication reuses cooked mesh");
	Check (physics.SyncCharacter (second, {}, mins, maxs), "rebase character scene with unchanged patch identities");
	Check (
		physics.Stats ().shapeBuilds == before.shapeBuilds && physics.Stats ().rebases > before.rebases,
		"basis-only rebase does not recook the welded character mesh");
	const auto trace = physics.Trace (second, {0, 0, -100});
	Check (!trace.hitUnknown && trace.fraction < 1, "ordinary query can interleave with character publication");
	const auto afterTrace = physics.Stats ().shapeBuilds;
	Check (physics.SyncCharacter (second, {}, mins, maxs), "restore character scene after ordinary query");
	Check (physics.Stats ().shapeBuilds == afterTrace, "ordinary query preserves retained welded character mesh");
	Check (physics.Stats ().shapeBytes <= 32 * 1024 * 1024, "combined shape memory remains within scene budget");
}
void AsyncAdapterTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world, {});
	JoltStreamWorld physics (world, true);
	const auto		position = At ({}, {132, 132, 200});
	auto			trace = physics.Trace (position, {0, 0, -180});
	Check (trace.hitUnknown, "cold asynchronous trace fails closed");
	auto readyTrace = [&] ()
	{
		const auto	deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
		TraceResult result;
		do
		{
			world.Pump (64);
			result = physics.Trace (position, {0, 0, -180});
			const auto stats = physics.Stats ();
			Check (stats.mainThreadCooks == 0, "live asynchronous queries never cook meshes on the main thread");
			Check (stats.pendingJobs <= 64 && stats.pendingBytes <= 32 * 1024 * 1024, "Jolt worker queue remains bounded");
			if (!result.hitUnknown)
				return result;
			std::this_thread::sleep_for (std::chrono::milliseconds (1));
		} while (std::chrono::steady_clock::now () < deadline);
		throw std::runtime_error ("asynchronous trace readiness timed out: " + physics.LastError ());
	};
	trace = readyTrace ();
	Check (std::abs (trace.end.local[2] - 64) < .05 && physics.Stats ().workerBuilds > 0, "worker-cooked trace matches terrain");
	const auto builds = physics.Stats ().workerBuilds;
	for (int i = 0; i < 32; ++i)
		Check (!physics.Trace (position, {0, 0, -180}).hitUnknown, "warm asynchronous trace stays available");
	Check (physics.Stats ().workerBuilds == builds, "warm asynchronous traces reuse cooked shapes");
	Check (world.Edit (At ({}, {132, 132, 76}), {3, Solid}), "edit asynchronous terrain");
	Check (physics.Trace (position, {0, 0, -180}).hitUnknown, "edit invalidates old collision before asynchronous replacement");
	trace = readyTrace ();
	Check (trace.end.local[2] > 76 && physics.Stats ().workerBuilds > builds, "edited geometry publishes a fresh worker-cooked shape");
	// Queue another revision and destroy immediately: workers must join before
	// World snapshots and the process-wide Jolt registry are released.
	Check (world.Edit (At ({}, {140, 132, 76}), {3, Solid}), "queue revision before shutdown");
	physics.Trace (position, {0, 0, -180});
}
void PlanetWrapTests ()
{
	WorldConfig config;
	config.generator = Generator::Planet;
	config.planetSettings.globalResolution = 32;
	config.planetSettings.extentChunks = 512;
	World world (config);
	Load (world, {});
	JoltStreamWorld physics (world);
	auto			first = physics.Trace (At ({}, {128, 128, 200}), {0, 0, -128});
	auto			alias = physics.Trace (At ({512, -512, 0}, {128, 128, 200}), {0, 0, -128});
	Check (!first.hitUnknown && !alias.hitUnknown && std::abs (first.fraction - alias.fraction) < 1e-6, "wrapped alias uses identical planet collision shape");
	JoltStreamCharacter character (physics);
	Driver				player (character, At ({511, 0, 0}, {1000, 128, 200}));
	player.Land ();
	for (int i = 0; i < 60; ++i)
		player.Tick (160);
	Check (player.input.position.chunk.x == 0 && player.input.position.local[0] > 50 && player.input.grounded, "walking across the planet seam keeps support");
}
} // namespace
int main (int argc, char **argv)
{
	JPH::RegisterDefaultAllocator ();
	JPH::Factory::sInstance = new JPH::Factory;
	JPH::RegisterTypes ();
	int result = 0;
	try
	{
		const std::string_view group = argc > 1 ? argv[1] : "all";
		Check (group == "all" || group == "terrain" || group == "controller" || group == "wrap", "unknown test group");
		if (group == "all" || group == "terrain")
		{
			TerrainTests ();
			LegacyMovementTests ();
			RenderToggleTests ();
		}
		if (group == "all" || group == "controller")
		{
			ControllerTests ();
			LandingJumpTests ();
			SlopeLandingJumpTests ();
			AsyncCharacterTests ();
			AsyncAdapterTests ();
			SolidFloorTests ();
			CharacterCacheTests ();
		}
		if (group == "all" || group == "wrap")
			PlanetWrapTests ();
		std::cout << "Jolt streamed terrain/controller checks passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << "Jolt stream test failed: " << error.what () << '\n';
		result = 1;
	}
	JPH::UnregisterTypes ();
	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;
	return result;
}
