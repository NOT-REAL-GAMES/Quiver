/*
Copyright (C) 2024 Quiver Contributors

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
// jolt_physics.cpp -- Jolt Physics integration for collision detection

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>

#include <cstdarg>
#include <cstring>
#include <cfloat>

// Suppress Jolt warnings
JPH_SUPPRESS_WARNINGS

using namespace JPH;

// Include Quiver headers (quakedef.h is included via jolt_physics.h)
#include "jolt_physics.h"

//=============================================================================
// Jolt World Structure
//=============================================================================

struct jolt_world_s
{
	RefConst<Shape>  mesh_shape;      // The static world mesh shape
	Vec3             world_mins;      // World bounds
	Vec3             world_maxs;
};

//=============================================================================
// Global State
//=============================================================================

static bool             g_jolt_initialized = false;
static TempAllocator   *g_temp_allocator = nullptr;
static JobSystem       *g_job_system = nullptr;
static jolt_world_t    *g_active_world = nullptr;

//=============================================================================
// Jolt Callbacks
//=============================================================================

static void TraceImpl(const char *inFMT, ...)
{
	va_list list;
	va_start(list, inFMT);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), inFMT, list);
	va_end(list);

	// Route to Quake console
	Con_Printf("Jolt: %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char *inExpression, const char *inMessage, const char *inFile, unsigned int inLine)
{
	Con_Printf("Jolt Assert: %s:%u: (%s) %s\n", inFile, inLine, inExpression, inMessage ? inMessage : "");
	return true; // Break into debugger
}
#endif

//=============================================================================
// Initialization
//=============================================================================

void Jolt_Init(void)
{
	if (g_jolt_initialized)
		return;

	// Register allocation hook
	RegisterDefaultAllocator();

	// Install callbacks
	Trace = TraceImpl;
	JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)

	// Create factory
	Factory::sInstance = new Factory();

	// Register physics types
	RegisterTypes();

	// Create temp allocator (10 MB)
	g_temp_allocator = new TempAllocatorImpl(10 * 1024 * 1024);

	// Single-threaded job system (we only need collision queries, not simulation)
	g_job_system = new JobSystemSingleThreaded(cMaxPhysicsJobs);

	g_jolt_initialized = true;

	Con_Printf("Jolt Physics initialized\n");
}

void Jolt_Shutdown(void)
{
	if (!g_jolt_initialized)
		return;

	// Free active world
	if (g_active_world)
	{
		Jolt_FreeWorld(g_active_world);
		g_active_world = nullptr;
	}

	// Cleanup
	delete g_job_system;
	g_job_system = nullptr;

	delete g_temp_allocator;
	g_temp_allocator = nullptr;

	UnregisterTypes();

	delete Factory::sInstance;
	Factory::sInstance = nullptr;

	g_jolt_initialized = false;
}

//=============================================================================
// World Creation
//=============================================================================

jolt_world_t *Jolt_CreateWorld(vec3_t *vertices, int num_vertices, int *indices, int num_indices)
{
	if (!g_jolt_initialized)
	{
		Con_Printf("Jolt_CreateWorld: Jolt not initialized!\n");
		return nullptr;
	}

	if (num_vertices == 0 || num_indices == 0)
	{
		Con_Printf("Jolt_CreateWorld: No geometry provided\n");
		return nullptr;
	}

	Con_Printf("Jolt_CreateWorld: Building mesh from %d vertices, %d indices (%d triangles)\n",
	           num_vertices, num_indices, num_indices / 3);

	// Convert vertices to Jolt format
	VertexList jolt_vertices;
	jolt_vertices.reserve(num_vertices);

	Vec3 mins(FLT_MAX, FLT_MAX, FLT_MAX);
	Vec3 maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (int i = 0; i < num_vertices; i++)
	{
		Float3 v(vertices[i][0], vertices[i][1], vertices[i][2]);
		jolt_vertices.push_back(v);

		// Track bounds
		mins = Vec3::sMin(mins, Vec3(v));
		maxs = Vec3::sMax(maxs, Vec3(v));
	}

	// Convert indices to Jolt format
	IndexedTriangleList jolt_triangles;
	jolt_triangles.reserve(num_indices / 3);

	for (int i = 0; i < num_indices; i += 3)
	{
		IndexedTriangle tri(indices[i], indices[i + 1], indices[i + 2]);
		jolt_triangles.push_back(tri);
	}

	// Create mesh shape settings
	MeshShapeSettings mesh_settings(jolt_vertices, jolt_triangles);

	// Create the shape
	Shape::ShapeResult result = mesh_settings.Create();
	if (result.HasError())
	{
		Con_Printf("Jolt_CreateWorld: Failed to create mesh shape: %s\n", result.GetError().c_str());
		return nullptr;
	}

	// Allocate world structure
	jolt_world_t *world = new jolt_world_t;
	world->mesh_shape = result.Get();
	world->world_mins = mins;
	world->world_maxs = maxs;

	Con_Printf("Jolt_CreateWorld: World bounds (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)\n",
	           mins.GetX(), mins.GetY(), mins.GetZ(),
	           maxs.GetX(), maxs.GetY(), maxs.GetZ());

	return world;
}

void Jolt_FreeWorld(jolt_world_t *world)
{
	if (!world)
		return;

	if (world == g_active_world)
		g_active_world = nullptr;

	// RefConst will handle cleanup when deleted
	delete world;
}

void Jolt_SetActiveWorld(jolt_world_t *world)
{
	g_active_world = world;
}

jolt_world_t *Jolt_GetActiveWorld(void)
{
	return g_active_world;
}

//=============================================================================
// Collision Queries
//=============================================================================

qboolean Jolt_TraceBox(jolt_world_t *world, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace)
{
	// Initialize trace result
	memset(trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	trace->allsolid = false;
	trace->startsolid = false;
	VectorCopy(end, trace->endpos);

	if (!world || !world->mesh_shape)
	{
		return false;
	}

	// Calculate box half-extents
	Vec3 half_extents(
		(maxs[0] - mins[0]) * 0.5f,
		(maxs[1] - mins[1]) * 0.5f,
		(maxs[2] - mins[2]) * 0.5f
	);

	// Calculate box center offset (mins/maxs may not be centered)
	Vec3 center_offset(
		(maxs[0] + mins[0]) * 0.5f,
		(maxs[1] + mins[1]) * 0.5f,
		(maxs[2] + mins[2]) * 0.5f
	);

	// Create box shape for the trace
	BoxShapeSettings box_settings(half_extents);
	box_settings.SetEmbedded();
	Shape::ShapeResult box_result = box_settings.Create();
	if (box_result.HasError())
	{
		return false;
	}
	RefConst<Shape> box_shape = box_result.Get();

	// Convert positions to Jolt coordinates (apply center offset)
	Vec3 jolt_start(start[0] + center_offset.GetX(),
	                start[1] + center_offset.GetY(),
	                start[2] + center_offset.GetZ());
	Vec3 jolt_end(end[0] + center_offset.GetX(),
	              end[1] + center_offset.GetY(),
	              end[2] + center_offset.GetZ());
	Vec3 direction = jolt_end - jolt_start;
	float move_length = direction.Length();

	// Handle zero-length traces
	if (move_length < 0.001f)
	{
		// Point test - check if start position overlaps with world
		CollideShapeSettings collide_settings;
		AllHitCollisionCollector<CollideShapeCollector> collector;

		// Transforms
		Mat44 box_transform = Mat44::sTranslation(jolt_start);
		Mat44 world_transform = Mat44::sIdentity();

		// Use collision dispatch for shape vs shape collision
		SubShapeIDCreator id_creator1, id_creator2;
		CollisionDispatch::sCollideShapeVsShape(
			box_shape, world->mesh_shape.GetPtr(),
			Vec3::sReplicate(1.0f), Vec3::sReplicate(1.0f),
			box_transform, world_transform,
			id_creator1, id_creator2,
			collide_settings, collector
		);

		if (collector.HadHit())
		{
			trace->startsolid = true;
			trace->allsolid = true;
			trace->fraction = 0.0f;
			VectorCopy(start, trace->endpos);
			return true;
		}
		return false;
	}

	// Create shape cast in world space
	ShapeCast shape_cast = ShapeCast::sFromWorldTransform(
		box_shape,
		Vec3::sReplicate(1.0f),
		Mat44::sTranslation(jolt_start),
		direction
	);

	// Settings for the cast
	ShapeCastSettings cast_settings;
	cast_settings.mBackFaceModeTriangles = EBackFaceMode::CollideWithBackFaces;
	cast_settings.mBackFaceModeConvex = EBackFaceMode::CollideWithBackFaces;

	// Perform cast against world mesh using collision dispatch
	ClosestHitCollisionCollector<CastShapeCollector> collector;
	SubShapeIDCreator id_creator1, id_creator2;
	Mat44 world_transform = Mat44::sIdentity();

	CollisionDispatch::sCastShapeVsShapeWorldSpace(
		shape_cast, cast_settings,
		world->mesh_shape.GetPtr(),
		Vec3::sReplicate(1.0f),
		ShapeFilter(),
		world_transform,
		id_creator1, id_creator2,
		collector
	);

	if (collector.HadHit())
	{
		const ShapeCastResult &hit = collector.mHit;

		// Check for starting in solid
		if (hit.mFraction <= 0.0f)
		{
			trace->startsolid = true;
			trace->allsolid = true;
			trace->fraction = 0.0f;
			VectorCopy(start, trace->endpos);
			return true;
		}

		trace->fraction = hit.mFraction;

		// Calculate end position
		Vec3 hit_pos = jolt_start + direction * hit.mFraction - center_offset;
		trace->endpos[0] = hit_pos.GetX();
		trace->endpos[1] = hit_pos.GetY();
		trace->endpos[2] = hit_pos.GetZ();

		// Get collision normal
		Vec3 normal = -hit.mPenetrationAxis.Normalized();

		trace->plane.normal[0] = normal.GetX();
		trace->plane.normal[1] = normal.GetY();
		trace->plane.normal[2] = normal.GetZ();

		// Calculate plane distance
		trace->plane.dist = trace->endpos[0] * trace->plane.normal[0] +
		                    trace->endpos[1] * trace->plane.normal[1] +
		                    trace->endpos[2] * trace->plane.normal[2];

		trace->contents = CONTENTS_SOLID;
		return true;
	}

	return false;
}

qboolean Jolt_TraceLine(jolt_world_t *world, vec3_t start, vec3_t end, trace_t *trace)
{
	// Initialize trace result
	memset(trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	VectorCopy(end, trace->endpos);

	if (!world || !world->mesh_shape)
		return false;

	// Create ray in local space (world is at origin)
	Vec3 jolt_start(start[0], start[1], start[2]);
	Vec3 jolt_end(end[0], end[1], end[2]);
	Vec3 direction = jolt_end - jolt_start;

	RayCast ray(jolt_start, direction);

	// Perform ray cast - use the simple single-hit version
	SubShapeIDCreator id_creator;
	RayCastResult hit;
	hit.mFraction = 1.0f + FLT_EPSILON;  // Initialize to beyond end

	bool has_hit = world->mesh_shape->CastRay(ray, id_creator, hit);

	if (has_hit && hit.mFraction < 1.0f)
	{
		trace->fraction = hit.mFraction;

		// Calculate end position
		Vec3 hit_pos = jolt_start + direction * hit.mFraction;
		trace->endpos[0] = hit_pos.GetX();
		trace->endpos[1] = hit_pos.GetY();
		trace->endpos[2] = hit_pos.GetZ();

		// Get surface normal from the shape
		Vec3 normal = world->mesh_shape->GetSurfaceNormal(hit.mSubShapeID2, hit_pos);
		trace->plane.normal[0] = normal.GetX();
		trace->plane.normal[1] = normal.GetY();
		trace->plane.normal[2] = normal.GetZ();

		trace->plane.dist = trace->endpos[0] * trace->plane.normal[0] +
		                    trace->endpos[1] * trace->plane.normal[1] +
		                    trace->endpos[2] * trace->plane.normal[2];

		trace->contents = CONTENTS_SOLID;
		return true;
	}

	return false;
}

int Jolt_PointContents(jolt_world_t *world, vec3_t point)
{
	if (!world || !world->mesh_shape)
		return CONTENTS_EMPTY;

	// Create a tiny sphere at the point to test containment
	SphereShapeSettings sphere_settings(0.1f);
	sphere_settings.SetEmbedded();
	Shape::ShapeResult sphere_result = sphere_settings.Create();
	if (sphere_result.HasError())
		return CONTENTS_EMPTY;

	RefConst<Shape> sphere_shape = sphere_result.Get();

	// Check for collision at this point
	CollideShapeSettings collide_settings;
	AnyHitCollisionCollector<CollideShapeCollector> collector;

	Mat44 sphere_transform = Mat44::sTranslation(Vec3(point[0], point[1], point[2]));
	Mat44 world_transform = Mat44::sIdentity();

	SubShapeIDCreator id_creator1, id_creator2;
	CollisionDispatch::sCollideShapeVsShape(
		sphere_shape, world->mesh_shape.GetPtr(),
		Vec3::sReplicate(1.0f), Vec3::sReplicate(1.0f),
		sphere_transform, world_transform,
		id_creator1, id_creator2,
		collide_settings, collector
	);

	if (collector.HadHit())
		return CONTENTS_SOLID;

	return CONTENTS_EMPTY;
}
