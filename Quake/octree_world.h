/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_OCTREE_WORLD_H
#define QUIVER_OCTREE_WORLD_H

#include "terrain_materials.h"
#include "planet_generation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quiver
{
constexpr int	 ChunkSize = 1024;
constexpr int	 VoxelSize = 8;
constexpr int	 VoxelsPerChunk = ChunkSize / VoxelSize;
constexpr size_t MaxCheckpointBytes = 64 * 1024 * 1024;
constexpr size_t MaxEntityPageBytes = 1024 * 1024;

struct ChunkKey
{
	int64_t	 x = 0, y = 0, z = 0;
	int64_t &operator[] (size_t axis);
	int64_t	 operator[] (size_t axis) const;
	bool	 operator== (const ChunkKey &) const = default;
};

// Hashes identify canonical, domain-separated immutable objects (SHA-256).
using ContentHash = std::array<uint8_t, 32>;
enum class ContentObjectType : uint8_t
{
	TerrainOverride = 1,
	SpatialNode = 2,
	SolidAsset = 3
};
struct ContentObject
{
	ContentHash			 hash{};
	ContentObjectType	 type = ContentObjectType::TerrainOverride;
	std::vector<uint8_t> bytes;
};
struct TerrainProof
{
	ContentHash					 root{};
	ChunkKey					 key;
	uint64_t					 revision = 0;
	std::vector<ContentObject>	 nodes;
	std::optional<ContentObject> overrideObject;
	std::vector<ContentObject>	 assets; // Unique canonical geometry objects referenced by solid operations.
};
ContentHash HashContent (ContentObjectType type, const std::vector<uint8_t> &bytes);
bool		ValidateTerrainProof (const TerrainProof &proof, std::string *error = nullptr);

struct WorldPosition
{
	ChunkKey			 chunk;
	std::array<float, 3> local{};
};

struct EntityPageInfo
{
	uint64_t id = 0;
	ChunkKey key;
	uint64_t revision = 0;
};
struct EntityPage
{
	EntityPageInfo		 info;
	std::vector<uint8_t> bytes;
};

// Failure leaves the destination unchanged. Normalized offsets are in [0, 1024).
bool Normalize (WorldPosition &position);
bool Add (const WorldPosition &position, const std::array<double, 3> &offset, WorldPosition &result);
bool Delta (const WorldPosition &to, const WorldPosition &from, std::array<double, 3> &result, double limit = 1048576.0);

enum Contents : int16_t
{
	Empty = -1,
	Solid = -2,
	Water = -3,
	Slime = -4,
	Lava = -5,
	Unknown = -6
};

struct Cell
{
	uint16_t material = 0;
	int16_t	 contents = Empty;
	bool	 operator== (const Cell &) const = default;
	bool	 IsSolid () const
	{
		return contents == Solid || contents == Unknown;
	}
};

class Octree
{
  public:
	Octree ();
	~Octree ();
	Octree (const Octree &);
	Octree &operator= (const Octree &);
	Octree (Octree &&) noexcept;
	Octree				&operator= (Octree &&) noexcept;
	Cell				 Get (int x, int y, int z) const;
	bool				 Set (int x, int y, int z, Cell cell);
	void				 Fill (Cell cell);
	// Coordinates and size are in voxels. Visits homogeneous leaves only.
	void				 Visit (const std::function<void (int, int, int, int, Cell)> &visitor) const;
	size_t				 NodeCount () const;
	std::vector<uint8_t> Encode () const;
	bool				 Decode (const std::vector<uint8_t> &bytes);

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	friend class World;
};

struct SolidDefinition;
struct Chunk
{
	ChunkKey											key;
	uint64_t											revision = 0;
	Octree												cells;
	std::shared_ptr<const std::vector<SolidDefinition>> solids;
};

struct MeshQuad
{
	// Chunk-local units. Counterclockwise winding viewed from outside:
	// use cyclic tangent axes (axis+1)%3, (axis+2)%3; reverse for sign < 0.
	// Sizes are powers of two times VoxelSize. To keep edges conforming,
	// triangulate sizes > VoxelSize with a center fan and perimeter points
	// spaced VoxelSize apart, including all four corners.
	std::array<float, 3> origin{};
	float				 size = 0;
	uint16_t			 material = 0;
	int16_t				 contents = Empty;
	uint8_t				 axis = 0;
	int8_t				 sign = 1;
};

struct MeshVertex
{
	std::array<float, 3> position{};
	std::array<float, 3> normal{0, 0, 1};
	TerrainMaterialBlend blend;
};
static_assert (sizeof (MeshVertex) == 32);

struct ChunkMesh
{
	ChunkKey				key;
	uint64_t				serial = 0; // Changes for both local edits and neighbor visibility changes.
	std::vector<MeshQuad>	quads;
	// Smooth terrain triangles are clockwise viewed from outside, ready for
	// the Vulkan terrain pipeline. Positions remain within the owning chunk.
	std::vector<MeshVertex> vertices;
	std::vector<uint32_t>	indices;
	// Render-only bridges from detailed boundary edges to the level-zero LOD
	// surface. Four contiguous index ranges: -X,+X,-Y,+Y. Never collide with
	// these skirts or draw a side adjoining another detailed column.
	std::vector<MeshVertex> transitionVertices;
	std::vector<uint32_t>	transitionIndices;
	std::array<uint32_t, 4> transitionCounts{};
	bool					smooth = false;
	size_t					Bytes () const
	{
		return quads.capacity () * sizeof (MeshQuad) + (vertices.capacity () + transitionVertices.capacity ()) * sizeof (MeshVertex) +
			   (indices.capacity () + transitionIndices.capacity ()) * sizeof (uint32_t);
	}
};

enum class Generator
{
	Empty,
	Terrain,
	Planet
};

// Exact procedural surface height in voxels, including the spawn plateau.
// Local column coordinates 0..128 include the shared chunk endpoint; unsigned
// lattice hashing stays defined even at signed int64 chunk limits. Invalid
// local columns return zero. This does not sample edits or generate an octree.
int TerrainHeightVoxels (uint64_t seed, int64_t chunkX, int64_t chunkY, int voxelX, int voxelY);

// Unrounded procedural height in world units. Local XY coordinates 0..1024
// include shared chunk endpoints. Invalid inputs return zero.
float TerrainSurfaceHeight (uint64_t seed, int64_t chunkX, int64_t chunkY, float localX, float localY);

// Smooth upward unit normal of the procedural surface. Local WORLD coordinates
// 0..1024 include chunk endpoints. Uses unrounded terrain gradients at a fixed
// scale independent of mesh LOD; invalid inputs return the flat normal (0,0,1).
// Edits are not sampled. No absolute floating world coordinates are constructed.
std::array<float, 3> TerrainSurfaceNormal (uint64_t seed, int64_t chunkX, int64_t chunkY, float localX, float localY);

struct WorldConfig
{
	Generator								 generator = Generator::Terrain;
	uint64_t								 seed = 1;
	PlanetSettings							 planetSettings;
	std::function<void (uint32_t, uint32_t)> generationProgress;
	std::shared_ptr<PlanetGenerator>		 planet;
	// Presentation only. Terrain collision always uses the smooth density surface.
	bool									 smoothTerrain = true;
	size_t									 residentLimit = 128;
	size_t									 pendingLimit = 64;
	size_t									 residentBytes = 256 * 1024 * 1024;
	size_t									 meshBytes = 128 * 1024 * 1024;
	// Empty means transient. A path enables durable SQLite persistence.
	std::string								 databasePath;
};

// Periodic worlds keep storage keys canonical while offsets use the nearest image.
ChunkKey CanonicalWorldKey (const WorldConfig &config, ChunkKey key);
bool	 WorldDelta (const WorldConfig &config, const WorldPosition &to, const WorldPosition &from, std::array<double, 3> &result, double limit = 1048576.0);
bool	 ApplyWorldDescriptor (WorldConfig &config, const std::vector<uint8_t> &descriptor, std::string *error = nullptr);

struct TraceResult
{
	double				 fraction = 1.0;
	std::array<float, 3> normal{};
	WorldPosition		 end;
	bool				 startSolid = false;
	bool				 allSolid = false;
	bool				 hitUnknown = false;
	Cell				 cell;
};

struct WorldStats
{
	size_t	 residentChunks = 0;
	size_t	 pendingChunks = 0;
	size_t	 pendingMeshes = 0;
	size_t	 pendingEdits = 0;
	size_t	 pendingEditBytes = 0;
	size_t	 pendingEntityPages = 0;
	size_t	 pendingEntityPageBytes = 0;
	size_t	 meshQuads = 0;
	size_t	 meshTriangles = 0;
	size_t	 octreeNodes = 0;
	size_t	 residentBytes = 0;
	size_t	 meshBytes = 0;
	size_t	 collisionPatches = 0;
	size_t	 collisionBytes = 0;
	uint64_t collisionBuilds = 0;
	uint64_t collisionSyncBuilds = 0, collisionWorkerBuilds = 0;
	size_t	 collisionPending = 0, collisionPendingBytes = 0;
	uint64_t collisionStaleResults = 0;
};

// Immutable, bounded collision geometry. Vertices remain chunk-local; patches
// own disjoint surface triangles. Unknown patches represent solid 128-unit boxes.
struct CollisionPatchSnapshot
{
	ChunkKey						 key;
	std::array<int, 3>				 patch{};
	uint64_t						 serial = 0;
	std::shared_ptr<const ChunkMesh> mesh;
	bool							 unknown = false;
	// Loaded authoritative data exists, but its current collision patch is not
	// ready. Pending patches must block until asynchronous publication.
	bool							 pending = false;
};

// Public methods run on the owning thread. Immutable chunk/mesh snapshots may be
// consumed by render workers. Generation/persistence and render meshing use
// separate owned workers; destruction drains both before releasing world state.
class World
{
  public:
	explicit World (WorldConfig config = {});
	~World ();
	World (const World &) = delete;
	World						&operator= (const World &) = delete;
	const WorldConfig			&Config () const;
	// Owning thread only. Invalidates cached geometry without changing cells.
	void						 SetSmoothTerrain (bool enabled);
	bool						 Request (ChunkKey key);
	bool						 Require (ChunkKey key, std::string *error = nullptr);
	void						 Pump (size_t maxCompletions = 16);
	// Pinned chunks cannot be evicted. The list must fit residentLimit.
	bool						 SetInterest (const std::vector<ChunkKey> &keys);
	std::shared_ptr<const Chunk> Find (ChunkKey key) const;
	std::vector<ChunkKey>		 ResidentKeys () const;
	Cell						 Sample (const WorldPosition &position) const;
	// Continuous terrain occupancy for movement/contents; Sample retains stored cells.
	Cell						 CollisionContents (const WorldPosition &position) const;
	// No render/storage waits. Returns false on invalid or excessive query bounds.
	bool						 CollisionPatches (
								const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
								std::vector<CollisionPatchSnapshot> &output) const;
	// Runtime path: only schedules extraction and publishes completed immutable
	// snapshots. Never generates density, extracts triangles, or waits. Missing
	// chunks are unknown; resident patches not yet prepared are pending. Serial
	// identifies the current authoritative neighborhood even while pending.
	bool CollisionPatchesAsync (
		const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
		std::vector<CollisionPatchSnapshot> &output) const;
	std::optional<uint64_t> CollisionSerial (ChunkKey key) const;
	// Cached density-lattice occupancy for live collision guards. Missing or
	// stale preparation returns Unknown and schedules its local patch.
	Cell					CollisionContentsCached (const WorldPosition &position) const;
	// Sweeps axis-aligned local bounds along offset. Unknown chunks are solid.
	TraceResult
	Trace (const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins = {}, const std::array<float, 3> &maxs = {}) const;
	// Stored voxel faces for edit selection and authoritative edit validation.
	TraceResult TraceVoxels (
		const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins = {}, const std::array<float, 3> &maxs = {}) const;
	// Durably commits before publishing the new revision or returning success.
	bool Edit (const WorldPosition &position, Cell cell, std::string *error = nullptr);
	using EditCompletion = std::function<void (bool success, uint64_t revision, const std::string &error)>;
	// Requires resident terrain and never waits for storage. A true return means
	// queued, not committed. Pump publishes the durable revision then invokes the
	// completion on the owning thread. Failure reports the unchanged revision.
	// One pending edit per chunk; destruction drains writes without callbacks.
	bool							 EditAsync (const WorldPosition &position, Cell cell, EditCompletion completion, std::string *error = nullptr);
	// Installs an authoritative network snapshot without writing client storage.
	// Older revisions are harmlessly ignored; edits invalidate adjoining meshes.
	bool							 Install (const Chunk &chunk, std::string *error = nullptr);
	// Nonblocking by default; an older cached mesh can be returned while rebuilding.
	std::shared_ptr<const ChunkMesh> Mesh (ChunkKey key, bool wait = false);
	// Last measured CPU size, retained across cache eviction. Unknown until built
	// or after a mesher change; failed builds report a size exceeding the budget.
	std::optional<size_t>			 MeshFootprint (ChunkKey key) const;
	bool							 SaveAs (const std::string &databasePath, std::string *error = nullptr);
	bool							 Flush (std::string *error = nullptr);
	// Feature surfaces use an eight-unit CSG brush; ordinary voxels use EditAsync.
	// CSG storage currently commits synchronously. Both paths defer completion
	// callbacks to Pump, so callers may install pending-request state on return.
	bool							 EditSurfaceAsync (const WorldPosition &position, Cell cell, EditCompletion completion, std::string *error = nullptr);
	// Commits every affected chunk in one transaction, then invalidates meshes.
	bool							 AddSolid (const SolidDefinition &solid, std::string *error = nullptr);
	// Explicit save/load commands wait for the worker. Payloads are opaque,
	// versioned and checksummed. No checkpoint loads as an empty vector;
	// failed loads leave the destination unchanged.
	// Save drains/publishes terrain/page operations first. Flush and Pump before
	// constructing a payload that must reflect those completion callbacks.
	bool							 SaveCheckpoint (const std::vector<uint8_t> &bytes, std::string *error = nullptr);
	bool							 LoadCheckpoint (std::vector<uint8_t> &bytes, std::string *error = nullptr);
	// Pages use stable nonzero IDs and compare-and-swap revisions. Expected zero
	// requires no current head; successful writes return a committed revision
	// strictly greater than any revision previously issued for this ID.
	// Deletes retain tombstones, so delayed requests cannot recreate an old ID.
	bool							 StoreEntityPage (
									uint64_t id, ChunkKey key, const std::vector<uint8_t> &bytes, uint64_t expectedRevision, uint64_t *newRevision = nullptr, std::string *error = nullptr);
	bool StoreEntityPageAsync (
		uint64_t id, ChunkKey key, const std::vector<uint8_t> &bytes, uint64_t expectedRevision, EditCompletion completion, std::string *error = nullptr);
	bool	 LoadEntityPage (uint64_t id, EntityPage &page, std::string *error = nullptr);
	bool	 DeleteEntityPage (uint64_t id, uint64_t expectedRevision, uint64_t *newRevision = nullptr, std::string *error = nullptr);
	bool	 DeleteEntityPageAsync (uint64_t id, uint64_t expectedRevision, EditCompletion completion, std::string *error = nullptr);
	uint64_t AllocateEntityPageId (std::string *error = nullptr);
	using EntityPageLoadCompletion = std::function<void (std::optional<EntityPage>, const std::string &error)>;
	using EntityPageListCompletion = std::function<void (std::vector<EntityPageInfo>, const std::string &error)>;
	// All async page operations share a64-job/64MiB budget, publish only in Pump,
	// and never invoke callbacks during destruction. Missing async loads return
	// nullopt with an empty error; synchronous loads return false and preserve output.
	bool LoadEntityPageAsync (uint64_t id, EntityPageLoadCompletion completion, std::string *error = nullptr);
	bool ListEntityPagesAsync (
		ChunkKey minimum, ChunkKey maximum, size_t limit, EntityPageListCompletion completion, std::string *error = nullptr, uint64_t afterId = 0);
	// Inclusive signed chunk bounds. At most 256 records; deterministic unsigned
	// ID order. An error returns an empty result and populates error/LastError.
	std::vector<EntityPageInfo> ListEntityPages (ChunkKey minimum, ChunkKey maximum, size_t limit, std::string *error = nullptr, uint64_t afterId = 0);
	// SaveCheckpoint atomically pins the current terrain root and page heads. Restore only after
	// validating its engine payload; ordinary checkpoint load retains current terrain.
	// Explicit RestoreSnapshot rolls terrain back. ID/revision clocks never rewind. Unreachable
	// runtime versions are pruned; named snapshots retain private content-addressed payloads.
	bool						RestoreEntityPagesCheckpoint (std::string *error = nullptr);
	// The descriptor includes canonical generator version, seed, dimensions and material identity.
	std::vector<uint8_t>		DescriptorBytes () const;
	ContentHash					CurrentRoot ();
	uint64_t					AuthorityEpoch ();
	bool						GetTerrainProof (ChunkKey key, TerrainProof &proof, std::string *error = nullptr);
	bool						ReadObject (const ContentHash &hash, ContentObject &object, std::string *error = nullptr);
	bool						InstallTerrainProof (const TerrainProof &proof, std::string *error = nullptr);
	bool						CreateSnapshot (const std::string &name, const std::vector<uint8_t> &checkpoint, std::string *error = nullptr);
	bool						LoadSnapshot (const std::string &name, std::vector<uint8_t> &checkpoint, std::string *error = nullptr);
	bool						RestoreSnapshot (const std::string &name, std::string *error = nullptr);
	std::vector<std::string>	ListSnapshots (std::string *error = nullptr);
	bool						DeleteSnapshot (const std::string &name, std::string *error = nullptr);
	bool						CollectGarbage (std::string *error = nullptr);
	WorldStats					Stats () const;
	const std::string		   &LastError () const;

  private:
	bool CollisionPatchesInternal (
		const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
		std::vector<CollisionPatchSnapshot> &output, bool asynchronous) const;
	TraceResult TraceInternal (
		const WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs, bool smooth) const;
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver
#endif
