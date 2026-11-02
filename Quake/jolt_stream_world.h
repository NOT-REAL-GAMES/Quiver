/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "octree_world.h"
#include <Jolt/Jolt.h>
#include <memory>

namespace JPH
{
class PhysicsSystem;
class TempAllocator;
} // namespace JPH

namespace quiver
{
struct JoltStreamStats
{
	size_t	 shapes = 0, shapeBytes = 0, bodies = 0;
	uint64_t shapeBuilds = 0, cacheHits = 0, rebases = 0;
	size_t	 pendingJobs = 0, pendingBytes = 0;
	uint64_t workerBuilds = 0, mainThreadCooks = 0, deferredQueries = 0;
};

struct JoltEntityProxy
{
	uint64_t			 id = 0;
	WorldPosition		 position;
	std::array<float, 3> mins{}, maxs{}, velocity{};
};

// A local, bounded Jolt scene. Register Jolt types before construction and keep
// the source world alive until destruction. All methods run on the owning thread.
class JoltStreamWorld
{
  public:
	static constexpr double Scale = 1.0 / 32.0;
	// Live adapters use asynchronous=true. Synchronous mode is an explicit
	// offline/testing convenience and may generate and cook collision geometry.
	explicit JoltStreamWorld (World &world, bool asynchronous = false);
	~JoltStreamWorld ();
	JoltStreamWorld (const JoltStreamWorld &) = delete;
	JoltStreamWorld &operator= (const JoltStreamWorld &) = delete;

	// Publish exact patches and solid barriers for unknown terrain. False means
	// invalid input or an exhausted budget; callers must stop in that case.
	// Rebasing changes Origin, so convert authoritative positions after Sync.
	bool Sync (const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs);
	// CharacterVirtual needs adjacency across streamed patch boundaries. Weld
	// its active terrain into one immutable mesh, reused until source identities
	// or revisions change. Unknown barriers and entity proxies remain separate.
	// Async mode also returns false with Pending() while replacement geometry
	// is prepared; callers defer movement and preserve their input state.
	bool SyncCharacter (const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs);
	TraceResult
	Trace (const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins = {}, const std::array<float, 3> &maxs = {});
	// Positions/velocities are Quake units; boxes remain axis aligned. Call before
	// Sync. IDs become body user data (zero is reserved for terrain).
	void SetEntityProxies (std::vector<JoltEntityProxy> proxies);
	bool IsAsync () const;
	bool Pending () const;
	Cell Contents (const WorldPosition &position) const;

	JPH::PhysicsSystem	&Physics ();
	JPH::TempAllocator	&Allocator ();
	World				&Source ();
	const WorldPosition &Origin () const;
	JPH::RVec3			 ToLocal (const WorldPosition &position) const;
	WorldPosition		 FromLocal (JPH::RVec3Arg position) const;
	JoltStreamStats		 Stats () const;
	const std::string	&LastError () const;

  private:
	bool SyncInternal (
		const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs, bool character);
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver
