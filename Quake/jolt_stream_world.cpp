/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "jolt_stream_world.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace quiver
{
namespace
{
constexpr size_t   ShapeBudget = 32 * 1024 * 1024;
constexpr size_t   CacheLimit = 4096;
constexpr size_t   ProxyLimit = 128;
constexpr uint64_t UnknownBody = UINT64_MAX;
constexpr double   ContactEpsilon = 0.03125;
constexpr float	   PenetrationTolerance = 0.00001f;
using PatchKey = std::array<int64_t, 6>;

JPH::Vec3 Vector (const std::array<double, 3> &v)
{
	return JPH::Vec3 (float (v[0] * JoltStreamWorld::Scale), float (v[1] * JoltStreamWorld::Scale), float (v[2] * JoltStreamWorld::Scale));
}

class Layers final : public JPH::BroadPhaseLayerInterface
{
  public:
	JPH::uint GetNumBroadPhaseLayers () const override
	{
		return 1;
	}
	JPH::BroadPhaseLayer GetBroadPhaseLayer (JPH::ObjectLayer) const override
	{
		return JPH::BroadPhaseLayer (0);
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName (JPH::BroadPhaseLayer) const override
	{
		return "streamed world";
	}
#endif
};

class EnteringShapeCollector final : public JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector>
{
  public:
	EnteringShapeCollector (JPH::Vec3Arg direction, JPH::RVec3Arg base) : direction (direction), base (base) {}
	void OnBody (const JPH::Body &value) override
	{
		body = &value;
	}
	void AddHit (const JPH::ShapeCastResult &hit) override
	{
		// An open patch boundary can supply a horizontal edge normal even when
		// the hull only rests on its floor. The actual outward triangle plane
		// determines whether this surface is being entered; neighboring patches
		// must not turn a coplanar seam into a wall.
		if (body && body->GetShape ()->GetType () == JPH::EShapeType::Mesh)
		{
			const auto outward = body->GetWorldSpaceSurfaceNormal (hit.mSubShapeID2, base + hit.mContactPointOn2);
			if (direction.Dot (outward) >= -1e-8f)
				return;
		}
		// A tangent or separating contact at fraction zero must permit sliding.
		if (hit.mPenetrationDepth <= PenetrationTolerance && direction.Dot (hit.mPenetrationAxis) <= 1e-8f)
			return;
		JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector>::AddHit (hit);
	}

  private:
	JPH::Vec3		 direction;
	JPH::RVec3		 base;
	const JPH::Body *body = nullptr;
};

using Signature = std::vector<std::pair<PatchKey, uint64_t>>;
constexpr size_t CookBudget = 32 * 1024 * 1024;
constexpr size_t CookQueueLimit = 64;
struct CookPart
{
	std::shared_ptr<const ChunkMesh> mesh;
	std::array<double, 3>			 delta{};
};
JPH::RefConst<JPH::Shape> CookShape (const std::vector<CookPart> &parts)
{
	JPH::VertexList							 vertices;
	JPH::IndexedTriangleList				 triangles;
	std::map<std::array<float, 3>, uint32_t> welded;
	for (const auto &part : parts)
	{
		const auto			 &mesh = *part.mesh;
		const auto			 &delta = part.delta;
		std::vector<uint32_t> indices;
		indices.reserve (mesh.vertices.size ());
		for (const auto &vertex : mesh.vertices)
		{
			std::array<float, 3> point;
			for (int a = 0; a < 3; ++a)
			{
				point[a] = float ((double (vertex.position[a]) + delta[a]) * JoltStreamWorld::Scale);
				if (!std::isfinite (point[a]))
					throw std::runtime_error ("Nonfinite streamed collision vertex");
			}
			// Rendering duplicates positions for material/normal attributes.
			// Exact welding also joins neighboring patches without moving
			// their surfaces or rounding hard construction features.
			auto [it, inserted] = welded.emplace (point, uint32_t (vertices.size ()));
			if (inserted)
				vertices.emplace_back (point[0], point[1], point[2]);
			indices.push_back (it->second);
		}
		for (size_t i = 0; i + 2 < mesh.indices.size (); i += 3)
		{
			if (mesh.indices[i] >= indices.size () || mesh.indices[i + 1] >= indices.size () || mesh.indices[i + 2] >= indices.size ())
				throw std::runtime_error ("Invalid streamed collision triangle");
			// Vulkan terrain is clockwise; Jolt expects outward CCW triangles.
			triangles.emplace_back (indices[mesh.indices[i]], indices[mesh.indices[i + 2]], indices[mesh.indices[i + 1]], 0);
		}
	}
	JPH::MeshShapeSettings settings (std::move (vertices), std::move (triangles));
	settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
	auto result = settings.Create ();
	if (result.HasError ())
		throw std::runtime_error (result.GetError ().c_str ());
	return result.Get ();
}
struct CookJob
{
	Signature			  signature;
	WorldPosition		  anchor;
	std::vector<CookPart> parts;
	bool				  connected = false;
	size_t				  bytes = 0;
};
struct CookResult
{
	Signature				  signature;
	WorldPosition			  anchor;
	JPH::RefConst<JPH::Shape> shape;
	bool					  connected = false;
	size_t					  bytes = 0;
	std::string				  error;
};
// Only immutable snapshots enter this worker. No World, PhysicsSystem, BodyInterface,
// or mutable adapter state is accessed here. Destruction joins before Jolt shuts down.
class Cooker
{
  public:
	Cooker () : thread ([this] { Run (); }) {}
	~Cooker ()
	{
		{
			std::lock_guard lock (mutex);
			stop = true;
			queued.clear ();
		}
		wake.notify_one ();
		thread.join ();
	}
	bool Enqueue (CookJob job)
	{
		std::lock_guard lock (mutex);
		auto			same = [&] (const auto &value) { return value.connected == job.connected && value.signature == job.signature; };
		if ((running && same (*running)) || std::any_of (queued.begin (), queued.end (), same) || std::any_of (finished.begin (), finished.end (), same))
			return true;
		for (auto it = queued.begin (); it != queued.end ();)
			if (it->connected == job.connected && (job.connected || it->signature.front ().first == job.signature.front ().first))
			{
				bytes -= it->bytes;
				it = queued.erase (it);
			}
			else
				++it;
		if (queued.size () + finished.size () + size_t (running.has_value ()) >= CookQueueLimit || job.bytes > CookBudget - std::min (bytes, CookBudget))
			return false;
		bytes += job.bytes;
		if (job.connected)
			queued.push_front (std::move (job));
		else
			queued.push_back (std::move (job));
		wake.notify_one ();
		return true;
	}
	std::deque<CookResult> Take ()
	{
		std::lock_guard		   lock (mutex);
		std::deque<CookResult> results;
		results.swap (finished);
		for (const auto &result : results)
			bytes -= result.bytes;
		return results;
	}
	std::pair<size_t, size_t> Status () const
	{
		std::lock_guard lock (mutex);
		return {queued.size () + finished.size () + size_t (running.has_value ()), bytes};
	}

  private:
	void Run ()
	{
		for (;;)
		{
			CookJob job;
			{
				std::unique_lock lock (mutex);
				wake.wait (lock, [&] { return stop || !queued.empty (); });
				if (stop)
					return;
				job = std::move (queued.front ());
				queued.pop_front ();
				running.emplace ();
				running->connected = job.connected;
				running->signature = job.signature;
			}
			CookResult result;
			result.signature = job.signature;
			result.anchor = job.anchor;
			result.connected = job.connected;
			try
			{
				if (!job.parts.empty ())
					result.shape = CookShape (job.parts);
				if (result.shape)
					result.bytes = result.shape->GetStats ().mSizeBytes;
			}
			catch (const std::exception &e)
			{
				result.error = e.what ();
			}
			job.parts.clear ();
			{
				std::lock_guard lock (mutex);
				bytes -= job.bytes;
				running.reset ();
				if (stop)
					return;
				if (result.bytes <= CookBudget - std::min (bytes, CookBudget))
				{
					bytes += result.bytes;
					finished.push_back (std::move (result));
				}
			}
		}
	}
	mutable std::mutex		mutex;
	std::condition_variable wake;
	std::deque<CookJob>		queued;
	std::deque<CookResult>	finished;
	std::optional<CookJob>	running;
	size_t					bytes = 0;
	bool					stop = false;
	std::thread				thread;
};

class ConvexBodyFilter final : public JPH::BodyFilter
{
  public:
	bool ShouldCollideLocked (const JPH::Body &body) const override
	{
		return body.GetShape ()->GetType () == JPH::EShapeType::Convex;
	}
};
} // namespace

struct JoltStreamWorld::Impl
{
	struct CachedShape
	{
		JPH::RefConst<JPH::Shape> shape;
		JPH::BodyID				  body;
		uint64_t				  serial = 0, touched = 0;
		size_t					  bytes = 0;
		bool					  unknown = false;
	};
	struct ProxyBody
	{
		JPH::BodyID			 body;
		std::array<float, 3> mins{}, maxs{};
	};
	World									  &world;
	Layers									   layers;
	JPH::ObjectVsBroadPhaseLayerFilter		   broadFilter;
	JPH::ObjectLayerPairFilter				   pairFilter;
	JPH::PhysicsSystem						   physics;
	JPH::TempAllocatorMalloc				   allocator;
	WorldPosition							   origin;
	bool									   initialized = false;
	std::map<PatchKey, CachedShape>			   cache;
	std::set<PatchKey>						   active;
	JPH::RefConst<JPH::Shape>				   connectedShape;
	JPH::BodyID								   connectedBody;
	WorldPosition							   connectedAnchor;
	std::vector<std::pair<PatchKey, uint64_t>> connectedSignature;
	size_t									   connectedBytes = 0;
	std::map<uint64_t, ProxyBody>			   proxyBodies;
	std::vector<JoltEntityProxy>			   proxies;
	uint64_t								   clock = 0;
	JoltStreamStats							   stats;
	std::string								   error;

	bool					asynchronous = false, pending = false;
	std::unique_ptr<Cooker> cooker;
	Signature				desiredConnected;

	explicit Impl (World &source, bool async) : world (source), asynchronous (async)
	{
		if (!JPH::Factory::sInstance)
			throw std::runtime_error ("Jolt types must be registered before creating a streamed world");
		physics.Init (CacheLimit + ProxyLimit, 0, 1024, 1024, layers, broadFilter, pairFilter);
		physics.SetGravity (JPH::Vec3::sZero ());
	}
	~Impl ()
	{
		cooker.reset ();
		Remove (connectedBody);
		for (auto &[key, shape] : cache)
			Remove (shape.body);
		for (auto &[id, proxy] : proxyBodies)
			Remove (proxy.body);
	}
	void Remove (JPH::BodyID &id)
	{
		if (id.IsInvalid ())
			return;
		auto &bodies = physics.GetBodyInterface ();
		if (bodies.IsAdded (id))
			bodies.RemoveBody (id);
		bodies.DestroyBody (id);
		id = {};
	}
	JPH::RVec3 Local (const WorldPosition &position) const
	{
		std::array<double, 3> delta;
		if (!WorldDelta (world.Config (), position, origin, delta, 2097152))
			throw std::runtime_error ("Jolt position is outside the local scene");
		return JPH::RVec3 (Vector (delta));
	}
	JPH::RVec3 Placement (const PatchKey &key, bool unknown) const
	{
		WorldPosition at{{key[0], key[1], key[2]}, {}};
		if (unknown)
			for (int axis = 0; axis < 3; ++axis)
				at.local[axis] = float (key[axis + 3] * 128 + 64);
		return Local (at);
	}
	static PatchKey Key (const CollisionPatchSnapshot &patch)
	{
		return {patch.key.x, patch.key.y, patch.key.z, patch.patch[0], patch.patch[1], patch.patch[2]};
	}
	bool Valid (const Signature &signature) const
	{
		for (const auto &[key, serial] : signature)
		{
			auto revision = world.CollisionSerial ({key[0], key[1], key[2]});
			if (!revision || *revision != serial)
				return false;
		}
		return true;
	}
	void DrainCooked ()
	{
		if (!cooker)
			return;
		for (auto &result : cooker->Take ())
		{
			if (!Valid (result.signature) || (result.connected && result.signature != desiredConnected))
				continue;
			if (!result.error.empty ())
				throw std::runtime_error (result.error);
			if (result.shape)
			{
				++stats.shapeBuilds;
				++stats.workerBuilds;
			}
			if (result.connected)
			{
				Remove (connectedBody);
				connectedShape = nullptr;
				connectedBytes = 0;
				std::set<PatchKey> barriers;
				for (const auto &key : active)
					if (auto it = cache.find (key); it != cache.end () && it->second.unknown)
						barriers.insert (key);
				Trim (barriers, result.bytes, true);
				if (stats.shapeBytes + result.bytes > ShapeBudget || cache.size () + size_t (result.shape != nullptr) > CacheLimit)
					throw std::runtime_error ("Asynchronous connected terrain exceeds shape budget");
				connectedShape = std::move (result.shape);
				connectedBytes = result.bytes;
				connectedSignature = std::move (result.signature);
				connectedAnchor = result.anchor;
			}
			else
			{
				const auto &[key, serial] = result.signature.front ();
				auto found = cache.find (key);
				if (found != cache.end ())
				{
					Remove (found->second.body);
					stats.shapeBytes -= found->second.bytes;
					cache.erase (found);
				}
				Trim (active, result.bytes);
				if (stats.shapeBytes + connectedBytes + result.bytes > ShapeBudget || cache.size () + size_t (connectedShape != nullptr) + 1 > CacheLimit)
					continue;
				CachedShape entry;
				entry.shape = std::move (result.shape);
				entry.bytes = result.bytes;
				entry.serial = serial;
				entry.touched = ++clock;
				stats.shapeBytes += entry.bytes;
				cache.emplace (key, std::move (entry));
			}
		}
	}
	bool Covers (const CollisionPatchSnapshot &patch) const
	{
		return std::binary_search (connectedSignature.begin (), connectedSignature.end (), std::pair{Key (patch), patch.serial});
	}
	void RequestConnected (const std::vector<CollisionPatchSnapshot> &patches)
	{
		std::vector<const CollisionPatchSnapshot *> surfaces;
		for (const auto &patch : patches)
		{
			if (patch.unknown)
				continue;
			if (patch.pending || !patch.mesh)
				return;
			surfaces.push_back (&patch);
		}
		if (surfaces.empty ())
			return;
		std::sort (surfaces.begin (), surfaces.end (), [] (auto a, auto b) { return Key (*a) < Key (*b); });
		Signature signature;
		for (auto surface : surfaces)
			signature.emplace_back (Key (*surface), surface->serial);
		if (signature == connectedSignature)
			return;
		auto job = MakeJob (surfaces, WorldPosition{surfaces.front ()->key, {}}, true);
		if (job.bytes > CookBudget)
			throw std::runtime_error ("Asynchronous connected snapshot budget exceeded");
		if (cooker->Enqueue (std::move (job)))
			desiredConnected = std::move (signature);
	}
	void PrepareAsync (
		const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs, bool character,
		std::vector<CollisionPatchSnapshot> &patches)
	{
		if (!cooker)
			cooker = std::make_unique<Cooker> ();
		DrainCooked ();
		if (!connectedSignature.empty () && !Valid (connectedSignature))
		{
			Remove (connectedBody);
			connectedShape = nullptr;
			connectedBytes = 0;
			connectedSignature.clear ();
		}
		if (!world.CollisionPatchesAsync (position, offset, mins, maxs, patches))
			throw std::runtime_error ("Asynchronous collision query exceeded its bounds");
		if (character)
		{
			// Prepare a larger bubble ahead of movement. Only the requested query
			// below can defer movement; unavailable speculative patches never do.
			std::array<float, 3>  lower, upper;
			std::array<double, 3> ahead;
			for (int a = 0; a < 3; ++a)
			{
				lower[a] = mins[a] - 128;
				upper[a] = maxs[a] + 128;
				ahead[a] = std::clamp (offset[a] * 48, -512.0, 512.0);
			}
			std::vector<CollisionPatchSnapshot> future;
			bool								futureReady = world.CollisionPatchesAsync (position, ahead, lower, upper, future);
			for (const auto &patch : future)
				futureReady &= patch.unknown || (!patch.pending && patch.mesh != nullptr);
			if (futureReady)
				RequestConnected (future);
			else
				RequestConnected (patches);
			for (auto &patch : patches)
				if (!patch.unknown && !Covers (patch))
				{
					pending = true;
					patch.unknown = true;
					patch.pending = true;
				}
		}
		else
		{
			for (auto &patch : patches)
			{
				if (patch.unknown)
					continue;
				auto found = cache.find (Key (patch));
				if (found != cache.end () && !found->second.unknown && found->second.serial == patch.serial)
					continue;
				if (!patch.pending && patch.mesh)
				{
					if (patch.mesh->indices.empty ())
						continue;
					auto job = MakeJob ({&patch}, WorldPosition{patch.key, {}}, false);
					if (job.bytes > CookBudget)
						throw std::runtime_error ("Asynchronous patch snapshot budget exceeded");
					cooker->Enqueue (std::move (job));
				}
				patch.unknown = true;
				patch.pending = true;
			}
		}
	}
	CookJob MakeJob (const std::vector<const CollisionPatchSnapshot *> &patches, const WorldPosition &anchor, bool connected)
	{
		CookJob job;
		job.anchor = anchor;
		job.connected = connected;
		for (const auto *patch : patches)
		{
			const PatchKey key{patch->key.x, patch->key.y, patch->key.z, patch->patch[0], patch->patch[1], patch->patch[2]};
			job.signature.emplace_back (key, patch->serial);
			if (!patch->mesh || patch->mesh->indices.empty ())
				continue;
			CookPart part;
			part.mesh = patch->mesh;
			if (!WorldDelta (world.Config (), WorldPosition{patch->key, {}}, anchor, part.delta, 2097152))
				throw std::runtime_error ("Connected terrain exceeds its local frame");
			job.bytes += part.mesh->Bytes ();
			job.parts.push_back (std::move (part));
		}
		return job;
	}
	JPH::RefConst<JPH::Shape> CookMesh (const std::vector<const CollisionPatchSnapshot *> &patches, const WorldPosition &anchor)
	{
		if (asynchronous)
			throw std::logic_error ("Live streamed terrain cannot cook on the owner thread");
		auto result = CookShape (MakeJob (patches, anchor, false).parts);
		++stats.shapeBuilds;
		++stats.mainThreadCooks;
		return result;
	}
	CachedShape CreateShape (const CollisionPatchSnapshot &patch)
	{
		CachedShape entry;
		entry.serial = patch.serial;
		entry.unknown = patch.unknown;
		if (patch.unknown)
		{
			entry.shape = new JPH::BoxShape (JPH::Vec3::sReplicate (float (64 * Scale)), 0);
			++stats.shapeBuilds;
		}
		else if (patch.mesh && !patch.mesh->indices.empty ())
			entry.shape = CookMesh ({&patch}, WorldPosition{patch.key, {}});
		if (entry.shape)
			entry.bytes = entry.shape->GetStats ().mSizeBytes;
		return entry;
	}
	void PublishConnected (const std::vector<CollisionPatchSnapshot> &patches, const std::set<PatchKey> &required, bool rebase, std::vector<JPH::BodyID> &added)
	{
		if (!asynchronous)
		{
			std::vector<const CollisionPatchSnapshot *> surfaces;
			for (const auto &patch : patches)
				if (!patch.unknown && patch.mesh && !patch.mesh->indices.empty ())
					surfaces.push_back (&patch);
			auto key = [] (const CollisionPatchSnapshot *p) { return PatchKey{p->key.x, p->key.y, p->key.z, p->patch[0], p->patch[1], p->patch[2]}; };
			std::sort (surfaces.begin (), surfaces.end (), [&] (auto a, auto b) { return key (a) < key (b); });
			std::vector<std::pair<PatchKey, uint64_t>> signature;
			for (auto surface : surfaces)
				signature.emplace_back (key (surface), surface->serial);
			if (signature != connectedSignature)
			{
				Remove (connectedBody);
				connectedShape = nullptr;
				connectedBytes = 0;
				connectedSignature.clear ();
				if (!surfaces.empty ())
				{
					connectedAnchor = {surfaces.front ()->key, {}};
					auto		 shape = CookMesh (surfaces, connectedAnchor);
					const size_t bytes = shape->GetStats ().mSizeBytes;
					Trim (required, bytes, true);
					if (stats.shapeBytes + bytes > ShapeBudget || cache.size () + 1 > CacheLimit)
						throw std::runtime_error ("Connected terrain shape memory budget exceeded");
					connectedShape = std::move (shape);
					connectedBytes = bytes;
				}
				connectedSignature = std::move (signature);
			}
			else if (connectedShape)
				++stats.cacheHits;
		}
		if (!connectedShape)
			return;
		auto &bodies = physics.GetBodyInterface ();
		if (connectedBody.IsInvalid ())
		{
			JPH::BodyCreationSettings settings (connectedShape, Local (connectedAnchor), JPH::Quat::sIdentity (), JPH::EMotionType::Static, 0);
			auto					  body = bodies.CreateBody (settings);
			if (!body)
				throw std::runtime_error ("Jolt connected terrain body budget exhausted");
			connectedBody = body->GetID ();
			added.push_back (connectedBody);
		}
		else if (rebase)
			bodies.SetPosition (connectedBody, Local (connectedAnchor), JPH::EActivation::DontActivate);
	}
	void Trim (const std::set<PatchKey> &required, size_t bytes = 0, bool connected = false)
	{
		// An ordinary trace may need the entire cache budget. Keep the last
		// character mesh for cheap interleaving while it fits, then evict it
		// before rejecting an otherwise valid per-patch query.
		if (!connected && connectedBody.IsInvalid () && connectedShape &&
			(cache.size () + 1 >= CacheLimit || stats.shapeBytes + connectedBytes + bytes > ShapeBudget))
		{
			connectedShape = nullptr;
			connectedBytes = 0;
			connectedSignature.clear ();
		}
		while (!cache.empty () && (cache.size () + size_t (connected || connectedShape != nullptr) + size_t (!connected) > CacheLimit ||
								   stats.shapeBytes + connectedBytes + bytes > ShapeBudget))
		{
			auto oldest = cache.end ();
			for (auto it = cache.begin (); it != cache.end (); ++it)
				if (!required.contains (it->first) && (oldest == cache.end () || it->second.touched < oldest->second.touched))
					oldest = it;
			if (oldest == cache.end ())
				break;
			Remove (oldest->second.body);
			stats.shapeBytes -= oldest->second.bytes;
			cache.erase (oldest);
		}
	}
	void PublishProxies (std::vector<JPH::BodyID> &added)
	{
		if (proxies.size () > ProxyLimit)
			throw std::runtime_error ("Too many nearby Jolt entity proxies");
		std::set<uint64_t> active;
		auto			  &bodies = physics.GetBodyInterface ();
		for (const auto &proxy : proxies)
			if (!proxy.id || proxy.id == UnknownBody || !active.insert (proxy.id).second)
				throw std::runtime_error ("Invalid Jolt entity proxy ID");
		// Free departing proxies before allocating replacements, even when the
		// terrain currently fills its entire body budget.
		for (auto it = proxyBodies.begin (); it != proxyBodies.end ();)
			if (!active.contains (it->first))
			{
				Remove (it->second.body);
				it = proxyBodies.erase (it);
			}
			else
				++it;
		for (const auto &proxy : proxies)
		{
			std::array<double, 3> center{}, extent{}, velocity{};
			for (int a = 0; a < 3; ++a)
			{
				if (!std::isfinite (proxy.mins[a]) || !std::isfinite (proxy.maxs[a]) || !std::isfinite (proxy.velocity[a]) || proxy.mins[a] >= proxy.maxs[a])
					throw std::runtime_error ("Invalid Jolt entity proxy bounds");
				center[a] = (double (proxy.mins[a]) + proxy.maxs[a]) * .5;
				extent[a] = (double (proxy.maxs[a]) - proxy.mins[a]) * .5;
				velocity[a] = proxy.velocity[a];
			}
			WorldPosition worldCenter;
			if (!Add (proxy.position, center, worldCenter))
				throw std::runtime_error ("Invalid Jolt entity proxy position");
			auto &entry = proxyBodies[proxy.id];
			if (!entry.body.IsInvalid () && (entry.mins != proxy.mins || entry.maxs != proxy.maxs))
				Remove (entry.body);
			if (entry.body.IsInvalid ())
			{
				JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape (Vector (extent), 0);
				JPH::BodyCreationSettings settings (shape, Local (worldCenter), JPH::Quat::sIdentity (), JPH::EMotionType::Kinematic, 0);
				settings.mUserData = proxy.id;
				settings.mLinearVelocity = Vector (velocity);
				auto body = bodies.CreateBody (settings);
				if (!body)
					throw std::runtime_error ("Jolt body budget exhausted");
				entry = {body->GetID (), proxy.mins, proxy.maxs};
				added.push_back (entry.body);
			}
			else
				bodies.SetPositionRotationAndVelocity (entry.body, Local (worldCenter), JPH::Quat::sIdentity (), Vector (velocity), JPH::Vec3::sZero ());
		}
	}
	JPH::Vec3 Normal (const JPH::RayCastResult &hit, JPH::RVec3Arg at) const
	{
		JPH::BodyLockRead lock (physics.GetBodyLockInterface (), hit.mBodyID);
		return lock.Succeeded () ? lock.GetBody ().GetWorldSpaceSurfaceNormal (hit.mSubShapeID2, at) : JPH::Vec3::sAxisZ ();
	}
	bool Unknown (JPH::BodyID id) const
	{
		return !id.IsInvalid () && physics.GetBodyInterface ().GetUserData (id) == UnknownBody;
	}
};

JoltStreamWorld::JoltStreamWorld (World &world, bool asynchronous) : impl (std::make_unique<Impl> (world, asynchronous)) {}
JoltStreamWorld::~JoltStreamWorld () = default;

bool JoltStreamWorld::Sync (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs)
{
	return SyncInternal (position, offset, mins, maxs, false);
}
bool JoltStreamWorld::SyncCharacter (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs)
{
	return SyncInternal (position, offset, mins, maxs, true);
}
bool JoltStreamWorld::SyncInternal (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs, bool character)
{
	impl->error.clear ();
	impl->pending = false;
	try
	{
		WorldPosition anchor = position;
		if (!Normalize (anchor))
			throw std::runtime_error ("Invalid Jolt scene origin");
		anchor.chunk = CanonicalWorldKey (impl->world.Config (), anchor.chunk);
		anchor.local = {};
		std::vector<CollisionPatchSnapshot> patches;
		if (impl->asynchronous)
			impl->PrepareAsync (position, offset, mins, maxs, character, patches);
		else if (!impl->world.CollisionPatches (position, offset, mins, maxs, patches))
			throw std::runtime_error ("Streamed collision query exceeded its bounds");
		std::set<PatchKey> required;
		std::set<PatchKey> retained;
		for (const auto &patch : patches)
		{
			const PatchKey key{patch.key.x, patch.key.y, patch.key.z, patch.patch[0], patch.patch[1], patch.patch[2]};
			required.insert (key);
			if (!character || patch.unknown)
				retained.insert (key);
		}
		if (required.size () > CacheLimit)
			throw std::runtime_error ("Streamed collision shape budget exceeded");
		const bool rebase = !impl->initialized || anchor.chunk != impl->origin.chunk;
		impl->origin = anchor;
		impl->initialized = true;
		if (rebase)
			++impl->stats.rebases;
		auto &bodies = impl->physics.GetBodyInterface ();
		// Retained cooked shapes may number in the thousands; only the small
		// previously published bubble needs body updates on a warm query.
		for (const auto &key : impl->active)
			if (auto found = impl->cache.find (key); found != impl->cache.end ())
				if (!retained.contains (key))
					impl->Remove (found->second.body);
		if (!character)
			impl->Remove (impl->connectedBody);
		std::vector<JPH::BodyID> added;
		std::set<PatchKey>		 published;
		if (character)
			impl->PublishConnected (patches, retained, rebase, added);
		for (const auto &patch : patches)
		{
			if (character && !patch.unknown)
				continue;
			const PatchKey key{patch.key.x, patch.key.y, patch.key.z, patch.patch[0], patch.patch[1], patch.patch[2]};
			published.insert (key);
			auto found = impl->cache.find (key);
			if (found != impl->cache.end () && (found->second.serial != patch.serial || found->second.unknown != patch.unknown))
			{
				impl->Remove (found->second.body);
				impl->stats.shapeBytes -= found->second.bytes;
				impl->cache.erase (found);
				found = impl->cache.end ();
			}
			if (found == impl->cache.end ())
			{
				auto entry = impl->CreateShape (patch);
				impl->Trim (retained, entry.bytes);
				if (impl->stats.shapeBytes + impl->connectedBytes + entry.bytes > ShapeBudget ||
					impl->cache.size () + size_t (impl->connectedShape != nullptr) + 1 > CacheLimit)
					throw std::runtime_error ("Streamed collision shape memory budget exceeded");
				found = impl->cache.emplace (key, std::move (entry)).first;
				impl->stats.shapeBytes += found->second.bytes;
			}
			else
				++impl->stats.cacheHits;
			auto &entry = found->second;
			entry.touched = ++impl->clock;
			if (!entry.shape)
				continue;
			if (entry.body.IsInvalid ())
			{
				JPH::BodyCreationSettings settings (entry.shape, impl->Placement (key, entry.unknown), JPH::Quat::sIdentity (), JPH::EMotionType::Static, 0);
				settings.mUserData = entry.unknown ? UnknownBody : 0;
				auto body = bodies.CreateBody (settings);
				if (!body)
					throw std::runtime_error ("Jolt terrain body budget exhausted");
				entry.body = body->GetID ();
				added.push_back (entry.body);
			}
			else if (rebase)
				bodies.SetPosition (entry.body, impl->Placement (key, entry.unknown), JPH::EActivation::DontActivate);
		}
		impl->PublishProxies (added);
		if (!added.empty ())
		{
			auto state = bodies.AddBodiesPrepare (added.data (), int (added.size ()));
			bodies.AddBodiesFinalize (added.data (), int (added.size ()), state, JPH::EActivation::DontActivate);
		}
		impl->active = std::move (published);
		if (character && impl->pending)
		{
			++impl->stats.deferredQueries;
			return false;
		}
		return true;
	}
	catch (const std::exception &error)
	{
		impl->error = error.what ();
		impl->pending = false;
		// A failed publication must never leave a partially updated usable scene.
		impl->Remove (impl->connectedBody);
		for (auto &[key, entry] : impl->cache)
			impl->Remove (entry.body);
		for (auto &[id, entry] : impl->proxyBodies)
			impl->Remove (entry.body);
		impl->active.clear ();
		return false;
	}
}

void JoltStreamWorld::SetEntityProxies (std::vector<JoltEntityProxy> proxies)
{
	impl->proxies = std::move (proxies);
}
bool JoltStreamWorld::IsAsync () const
{
	return impl->asynchronous;
}
bool JoltStreamWorld::Pending () const
{
	return impl->pending;
}
Cell JoltStreamWorld::Contents (const WorldPosition &position) const
{
	return impl->asynchronous ? impl->world.CollisionContentsCached (position) : impl->world.CollisionContents (position);
}
JPH::PhysicsSystem &JoltStreamWorld::Physics ()
{
	return impl->physics;
}
JPH::TempAllocator &JoltStreamWorld::Allocator ()
{
	return impl->allocator;
}
World &JoltStreamWorld::Source ()
{
	return impl->world;
}
const WorldPosition &JoltStreamWorld::Origin () const
{
	return impl->origin;
}
JPH::RVec3 JoltStreamWorld::ToLocal (const WorldPosition &position) const
{
	return impl->Local (position);
}
WorldPosition JoltStreamWorld::FromLocal (JPH::RVec3Arg position) const
{
	WorldPosition result;
	if (!Add (impl->origin, {position.GetX () / Scale, position.GetY () / Scale, position.GetZ () / Scale}, result))
		throw std::runtime_error ("Invalid Jolt scene position");
	result.chunk = CanonicalWorldKey (impl->world.Config (), result.chunk);
	return result;
}
JoltStreamStats JoltStreamWorld::Stats () const
{
	auto stats = impl->stats;
	stats.shapes = impl->cache.size () + size_t (impl->connectedShape != nullptr);
	stats.shapeBytes += impl->connectedBytes;
	stats.bodies = impl->physics.GetNumBodies ();
	if (impl->cooker)
	{
		auto [jobs, bytes] = impl->cooker->Status ();
		stats.pendingJobs = jobs;
		stats.pendingBytes = bytes;
	}
	return stats;
}
const std::string &JoltStreamWorld::LastError () const
{
	return impl->error;
}

TraceResult
JoltStreamWorld::Trace (const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs)
try
{
	TraceResult result;
	result.end = position;
	if (!Add (position, offset, result.end) || !Sync (position, offset, mins, maxs))
	{
		result.fraction = 0;
		result.end = position;
		result.startSolid = result.allSolid = result.hitUnknown = true;
		result.cell = {0, Unknown};
		return result;
	}
	std::array<double, 3> centerOffset{}, halfExtent{};
	bool				  point = true;
	for (int axis = 0; axis < 3; ++axis)
	{
		centerOffset[axis] = (double (mins[axis]) + maxs[axis]) * .5;
		halfExtent[axis] = (double (maxs[axis]) - mins[axis]) * .5;
		point &= halfExtent[axis] == 0;
	}
	WorldPosition center;
	if (!Add (position, centerOffset, center))
		throw std::runtime_error ("Invalid trace center");
	const auto												  from = ToLocal (center);
	const auto												  direction = Vector (offset);
	const auto												 &query = impl->physics.GetNarrowPhaseQuery ();
	const Cell												  centerCell = Contents (center);
	JPH::AllHitCollisionCollector<JPH::CollidePointCollector> centerHits;
	query.CollidePoint (from, centerHits, {}, {}, ConvexBodyFilter{});
	const bool centerInside = centerCell.IsSolid () || centerHits.HadHit ();
	result.startSolid = centerInside;
	result.hitUnknown = centerCell.contents == Unknown;
	for (const auto &contact : centerHits.mHits)
		result.hitUnknown |= impl->Unknown (contact.mBodyID);
	if (result.startSolid)
		result.cell = centerCell;
	JPH::RefConst<JPH::Shape> shape;
	JPH::BodyID				  hitBody;
	JPH::Vec3				  normal = JPH::Vec3::sZero ();
	double					  fraction = 1;
	bool					  hit = false;
	if (point)
	{
		JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
		query.CastRay (JPH::RRayCast (from, direction), {}, collector);
		if (collector.HadHit ())
		{
			const auto &contact = collector.mHit;
			hit = true;
			fraction = contact.mFraction;
			hitBody = contact.mBodyID;
			normal = impl->Normal (contact, from + direction * contact.mFraction);
		}
	}
	else
	{
		shape = new JPH::BoxShape (Vector (halfExtent), 0);
		JPH::CollideShapeSettings overlapSettings;
		overlapSettings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
		overlapSettings.mCollisionTolerance = 1e-6f;
		JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> overlaps;
		query.CollideShape (shape, JPH::Vec3::sReplicate (1), JPH::RMat44::sTranslation (from), overlapSettings, from, overlaps);
		for (const auto &overlap : overlaps.mHits)
			if (overlap.mPenetrationDepth > PenetrationTolerance)
			{
				result.startSolid = true;
				result.hitUnknown |= impl->Unknown (overlap.mBodyID2);
			}
		JPH::ShapeCastSettings settings;
		settings.mCollisionTolerance = 1e-6f;
		settings.mReturnDeepestPoint = true;
		settings.mActiveEdgeMovementDirection = direction;
		EnteringShapeCollector collector (direction, from);
		query.CastShape (JPH::RShapeCast (shape, JPH::Vec3::sReplicate (1), JPH::RMat44::sTranslation (from), direction), settings, from, collector);
		if (collector.HadHit ())
		{
			const auto &contact = collector.mHit;
			hit = true;
			fraction = contact.mFraction;
			hitBody = contact.mBodyID2;
			normal = -contact.mPenetrationAxis.NormalizedOr (JPH::Vec3::sAxisZ ());
		}
	}
	if (result.startSolid)
	{
		// Mesh patches are open surfaces, so Jolt CollidePoint cannot determine
		// their interior. Classify the center from the authoritative field, then
		// use oriented Jolt ray crossings and forward/reverse hull casts to merge
		// the occupied intervals. This preserves Quake's allSolid/escape semantics.
		std::vector<std::pair<double, double>> intervals;
		if (shape && direction.LengthSq () > 0)
		{
			JPH::ShapeCastSettings settings;
			settings.SetBackFaceMode (JPH::EBackFaceMode::CollideWithBackFaces);
			settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
			settings.mCollisionTolerance = 1e-6f;
			JPH::AllHitCollisionCollector<JPH::CastShapeCollector> forward, backward;
			query.CastShape (JPH::RShapeCast (shape, JPH::Vec3::sReplicate (1), JPH::RMat44::sTranslation (from), direction), settings, from, forward);
			query.CastShape (
				JPH::RShapeCast (shape, JPH::Vec3::sReplicate (1), JPH::RMat44::sTranslation (from + direction), -direction), settings, from, backward);
			std::map<std::pair<uint32_t, uint32_t>, double> exits;
			for (const auto &contact : backward.mHits)
				exits[{contact.mBodyID2.GetIndexAndSequenceNumber (), contact.mSubShapeID2.GetValue ()}] = 1.0 - contact.mFraction;
			for (const auto &contact : forward.mHits)
			{
				auto exit = exits.find ({contact.mBodyID2.GetIndexAndSequenceNumber (), contact.mSubShapeID2.GetValue ()});
				if (exit != exits.end () && exit->second > contact.mFraction)
					intervals.emplace_back (contact.mFraction, exit->second);
			}
		}
		JPH::AllHitCollisionCollector<JPH::CastRayCollector> crossings;
		JPH::RayCastSettings								 raySettings;
		raySettings.SetBackFaceMode (JPH::EBackFaceMode::CollideWithBackFaces);
		raySettings.mTreatConvexAsSolid = false;
		query.CastRay (JPH::RRayCast (from, direction), raySettings, crossings);
		std::vector<std::pair<double, bool>> transitions;
		for (const auto &contact : crossings.mHits)
		{
			const auto n = impl->Normal (contact, from + direction * contact.mFraction);
			if (std::abs (direction.Dot (n)) > 1e-8f)
				transitions.emplace_back (contact.mFraction, direction.Dot (n) < 0);
		}
		std::sort (transitions.begin (), transitions.end ());
		bool   inside = centerInside;
		double begin = 0;
		for (size_t i = 0; i < transitions.size ();)
		{
			double t = transitions[i].first;
			bool   enter = false, leave = false;
			do
			{
				enter |= transitions[i].second;
				leave |= !transitions[i].second;
				++i;
			} while (i < transitions.size () && transitions[i].first <= t + 1e-6);
			if (enter == leave)
				continue;
			if (leave && inside)
			{
				intervals.emplace_back (begin, t);
				inside = false;
			}
			else if (enter && !inside)
			{
				begin = t;
				inside = true;
			}
		}
		if (inside)
			intervals.emplace_back (begin, 1);
		std::sort (intervals.begin (), intervals.end ());
		double covered = 0;
		for (const auto &[begin, end] : intervals)
		{
			if (begin > covered + 1e-6)
				break;
			covered = std::max (covered, end);
		}
		result.allSolid = direction.LengthSq () == 0 || covered >= 1 - 1e-6;
		result.fraction = 0;
		result.end = position;
		result.cell = {1, result.hitUnknown ? Unknown : Solid};
		return result;
	}
	if (hit)
	{
		double speed = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			result.normal[axis] = normal[axis];
			speed -= offset[axis] * normal[axis];
		}
		result.fraction = std::clamp (fraction - ContactEpsilon / std::max (speed, 1e-12), 0.0, 1.0);
		std::array<double, 3> move;
		for (int axis = 0; axis < 3; ++axis)
			move[axis] = offset[axis] * result.fraction;
		Add (position, move, result.end);
		result.hitUnknown = impl->Unknown (hitBody);
		result.cell = {1, result.hitUnknown ? Unknown : Solid};
	}
	return result;
}
catch (const std::exception &error)
{
	impl->error = error.what ();
	TraceResult result;
	result.end = position;
	result.fraction = 0;
	result.startSolid = result.allSolid = result.hitUnknown = true;
	result.cell = {0, Unknown};
	return result;
}
} // namespace quiver
