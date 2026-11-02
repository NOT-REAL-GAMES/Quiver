/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "octree_world.h"
#include "world_sha256.h"
#include <filesystem>
#include <set>
#include <map>
#include "terrain_mesher.h"
#include "terrain_collision.h"
#include "terrain_density.h"
#include "world_solids.h"
#include "terrain_features.h"
#include "planet_surface.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <sqlite3.h>

namespace quiver
{
namespace
{
constexpr size_t MaxMeshQuads = 1000000;
constexpr size_t MaxEncodedBytes = 16 * 1024 * 1024;
constexpr double CollisionEpsilon = 0.03125;

uint32_t CheckpointChecksum (const std::vector<uint8_t> &bytes)
{
	static constexpr auto table = []
	{
		std::array<uint32_t, 256> values{};
		for (uint32_t i = 0; i < values.size (); ++i)
		{
			uint32_t value = i;
			for (int bit = 0; bit < 8; ++bit)
				value = (value >> 1) ^ ((value & 1) ? UINT32_C (0xedb88320) : 0);
			values[i] = value;
		}
		return values;
	}();
	uint32_t checksum = UINT32_MAX;
	for (uint8_t byte : bytes)
		checksum = table[(checksum ^ byte) & 255] ^ (checksum >> 8);
	return ~checksum;
}

struct KeyHash
{
	size_t operator() (const ChunkKey &key) const
	{
		uint64_t h = 0x9e3779b97f4a7c15ULL;
		for (size_t i = 0; i != 3; ++i)
		{
			uint64_t v = static_cast<uint64_t> (key[i]);
			v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
			v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
			h ^= (v ^ (v >> 31)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
		}
		return static_cast<size_t> (h);
	}
};

bool CheckedAdd (int64_t a, int64_t b, int64_t &result)
{
	if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
		return false;
	result = a + b;
	return true;
}

bool ValidCell (Cell cell)
{
	return cell.contents >= Lava && cell.contents <= Empty;
}

bool Canonicalize (const WorldPosition &position, const std::array<double, 3> &offset, WorldPosition &result)
{
	WorldPosition candidate = position;
	for (size_t i = 0; i != 3; ++i)
	{
		const long double value = static_cast<long double> (position.local[i]) + offset[i];
		if (!std::isfinite (value))
			return false;
		long double chunks = std::floor (value / ChunkSize);
		// Both bounds are exact powers of two even where long double == double.
		if (chunks < -9223372036854775808.0L || chunks >= 9223372036854775808.0L)
			return false;
		int64_t chunk;
		if (!CheckedAdd (position.chunk[i], static_cast<int64_t> (chunks), chunk))
			return false;
		float local = static_cast<float> (value - chunks * ChunkSize);
		// Floating conversion may round the upper open boundary to exactly 1024.
		if (local >= ChunkSize)
		{
			if (!CheckedAdd (chunk, 1, chunk))
				return false;
			local = 0;
		}
		candidate.chunk[i] = chunk;
		candidate.local[i] = local;
	}
	result = candidate;
	return true;
}

struct Node
{
	Cell								 cell;
	std::unique_ptr<std::array<Node, 8>> children;

	Node () = default;
	Node (const Node &other) : cell (other.cell)
	{
		if (other.children)
			children = std::make_unique<std::array<Node, 8>> (*other.children);
	}
	Node &operator= (const Node &other)
	{
		if (this != &other)
		{
			Node replacement (other);
			*this = std::move (replacement);
		}
		return *this;
	}
	Node (Node &&) noexcept = default;
	Node &operator= (Node &&) noexcept = default;

	void Fill (Cell value)
	{
		cell = value;
		children.reset ();
	}

	void Split ()
	{
		children = std::make_unique<std::array<Node, 8>> ();
		for (auto &child : *children)
			child.cell = cell;
	}

	void Collapse ()
	{
		const Cell first = (*children)[0].cell;
		for (const auto &child : *children)
			if (child.children || child.cell != first)
				return;
		Fill (first);
	}

	bool Set (int x, int y, int z, int size, Cell value)
	{
		if (!children && cell == value)
			return false;
		if (size == 1)
		{
			Fill (value);
			return true;
		}
		if (!children)
			Split ();
		int	 half = size / 2;
		int	 index = (x >= half) | ((y >= half) << 1) | ((z >= half) << 2);
		bool changed = (*children)[index].Set (x % half, y % half, z % half, half, value);
		Collapse ();
		return changed;
	}

	Cell Get (int x, int y, int z, int size) const
	{
		if (!children)
			return cell;
		int half = size / 2;
		int index = (x >= half) | ((y >= half) << 1) | ((z >= half) << 2);
		return (*children)[index].Get (x % half, y % half, z % half, half);
	}

	void Visit (int x, int y, int z, int size, const std::function<void (int, int, int, int, Cell)> &visitor) const
	{
		if (!children)
		{
			visitor (x, y, z, size, cell);
			return;
		}
		int half = size / 2;
		for (int i = 0; i < 8; ++i)
			(*children)[i].Visit (x + (i & 1) * half, y + ((i >> 1) & 1) * half, z + ((i >> 2) & 1) * half, half, visitor);
	}

	size_t Count () const
	{
		size_t count = 1;
		if (children)
			for (const auto &child : *children)
				count += child.Count ();
		return count;
	}

	void Encode (std::vector<uint8_t> &bytes) const
	{
		bytes.push_back (children ? 1 : 0);
		if (children)
		{
			for (const auto &child : *children)
				child.Encode (bytes);
		}
		else
		{
			bytes.push_back (static_cast<uint8_t> (cell.material));
			bytes.push_back (static_cast<uint8_t> (cell.material >> 8));
			uint16_t contents = static_cast<uint16_t> (cell.contents);
			bytes.push_back (static_cast<uint8_t> (contents));
			bytes.push_back (static_cast<uint8_t> (contents >> 8));
		}
	}

	bool Decode (const std::vector<uint8_t> &bytes, size_t &cursor, int depth)
	{
		if (cursor >= bytes.size ())
			return false;
		uint8_t tag = bytes[cursor++];
		if (tag == 1)
		{
			if (depth >= 7)
				return false;
			Split ();
			for (auto &child : *children)
				if (!child.Decode (bytes, cursor, depth + 1))
					return false;
			Collapse ();
			return true;
		}
		if (tag != 0 || bytes.size () - cursor < 4)
			return false;
		Cell value;
		value.material = static_cast<uint16_t> (bytes[cursor] | (uint16_t (bytes[cursor + 1]) << 8));
		value.contents = static_cast<int16_t> (bytes[cursor + 2] | (uint16_t (bytes[cursor + 3]) << 8));
		cursor += 4;
		if (!ValidCell (value))
			return false;
		Fill (value);
		return true;
	}
};

void SqlCheck (int result, sqlite3 *db, const char *operation)
{
	if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
		throw std::runtime_error (std::string (operation) + ": " + sqlite3_errmsg (db));
}

void SqlExec (sqlite3 *db, const char *sql)
{
	SqlCheck (sqlite3_exec (db, sql, nullptr, nullptr, nullptr), db, sql);
}

struct Statement
{
	sqlite3_stmt *statement = nullptr;
	sqlite3		 *db;
	Statement (sqlite3 *database, const char *sql) : db (database)
	{
		SqlCheck (sqlite3_prepare_v2 (db, sql, -1, &statement, nullptr), db, "prepare");
	}
	~Statement ()
	{
		sqlite3_finalize (statement);
	}
	void Key (ChunkKey key)
	{
		for (int i = 0; i != 3; ++i)
			SqlCheck (sqlite3_bind_int64 (statement, i + 1, key[i]), db, "bind chunk");
	}
	void Id (int parameter, uint64_t id)
	{
		std::array<uint8_t, 8> bytes;
		for (int i = 0; i < 8; ++i)
			bytes[i] = uint8_t (id >> ((7 - i) * 8));
		SqlCheck (sqlite3_bind_blob (statement, parameter, bytes.data (), bytes.size (), SQLITE_TRANSIENT), db, "bind entity page ID");
	}
};

uint64_t ReadPageId (sqlite3_stmt *statement, int column)
{
	if (sqlite3_column_type (statement, column) != SQLITE_BLOB || sqlite3_column_bytes (statement, column) != 8)
		throw std::runtime_error ("invalid entity page ID");
	const auto *bytes = static_cast<const uint8_t *> (sqlite3_column_blob (statement, column));
	if (!bytes)
		throw std::runtime_error ("cannot read entity page ID");
	uint64_t id = 0;
	for (int i = 0; i < 8; ++i)
		id = (id << 8) | bytes[i];
	return id;
}

bool ValidPageBounds (ChunkKey minimum, ChunkKey maximum, size_t limit)
{
	return limit > 0 && limit <= 256 && minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
}

constexpr int TerrainSpacing = 32;
constexpr int TerrainGrid = VoxelsPerChunk / TerrainSpacing;

int TerrainLatticeValue (uint64_t seed, uint64_t x, uint64_t y)
{
	uint64_t hash = x * 0x9e3779b97f4a7c15ULL ^ y * 0xbf58476d1ce4e5b9ULL ^ seed;
	hash = (hash ^ (hash >> 30)) * 0x94d049bb133111ebULL;
	return 8 + static_cast<int> ((hash ^ (hash >> 31)) % 9);
}

int TerrainInterpolatedHeight (int64_t chunkX, int64_t chunkY, int voxelX, int voxelY, int h00, int h10, int h01, int h11)
{
	constexpr int Scale = TerrainSpacing * TerrainSpacing;
	int			  fx = voxelX % TerrainSpacing, fy = voxelY % TerrainSpacing;
	int			  low = h00 * (TerrainSpacing - fx) + h10 * fx;
	int			  high = h01 * (TerrainSpacing - fx) + h11 * fx;
	int			  height = low * (TerrainSpacing - fy) + high * fy;
	if (chunkX >= -1 && chunkX <= 0 && chunkY >= -1 && chunkY <= 0)
	{
		int radius = std::max (std::abs (int (chunkX) * VoxelsPerChunk + voxelX - 16), std::abs (int (chunkY) * VoxelsPerChunk + voxelY - 16));
		int weight = std::clamp (radius - 32, 0, 32);
		height = 8 * Scale + (height - 8 * Scale) * weight / 32;
	}
	return (height + Scale / 2) / Scale;
}

double TerrainContinuousHeight (uint64_t seed, int64_t chunkX, int64_t chunkY, double localX, double localY)
{
	constexpr double LatticeSize = TerrainSpacing * VoxelSize;
	const double	 gx = localX / LatticeSize, gy = localY / LatticeSize;
	const int		 bx = int (std::floor (gx)), by = int (std::floor (gy));
	const double	 fx = gx - bx, fy = gy - by;
	// Normal probes can cross a chunk edge. Carry in unsigned lattice space,
	// where hashing already wraps, instead of overflowing signed chunk keys.
	const uint64_t	 x = uint64_t (chunkX) * TerrainGrid + uint64_t (bx);
	const uint64_t	 y = uint64_t (chunkY) * TerrainGrid + uint64_t (by);
	const double	 low = TerrainLatticeValue (seed, x, y) * (1 - fx) + TerrainLatticeValue (seed, x + 1, y) * fx;
	const double	 high = TerrainLatticeValue (seed, x, y + 1) * (1 - fx) + TerrainLatticeValue (seed, x + 1, y + 1) * fx;
	double			 height = low * (1 - fy) + high * fy;
	if (chunkX >= -1 && chunkX <= 0 && chunkY >= -1 && chunkY <= 0)
	{
		const double radius = std::max (std::abs (double (chunkX) * ChunkSize + localX - 128), std::abs (double (chunkY) * ChunkSize + localY - 128));
		const double weight = std::clamp ((radius - 256) / 256, 0.0, 1.0);
		height = 8 + (height - 8) * weight;
	}
	return height * VoxelSize;
}
} // namespace

#include "world_merkle_codec.inc"

ChunkKey CanonicalWorldKey (const WorldConfig &config, ChunkKey key)
{
	if (config.generator == Generator::Planet)
	{
		const auto extent = config.planetSettings.extentChunks;
		if (extent <= 0)
			throw std::invalid_argument ("invalid planet extent");
		for (int axis = 0; axis < 2; ++axis)
		{
			key[axis] %= extent;
			if (key[axis] < 0)
				key[axis] += extent;
		}
	}
	return key;
}
bool WorldDelta (const WorldConfig &config, const WorldPosition &to, const WorldPosition &from, std::array<double, 3> &result, double limit)
{
	if (config.generator != Generator::Planet)
		return Delta (to, from, result, limit);
	auto a = to, b = from;
	if (!Normalize (a) || !Normalize (b))
		return false;
	a.chunk = CanonicalWorldKey (config, a.chunk);
	b.chunk = CanonicalWorldKey (config, b.chunk);
	const auto extent = config.planetSettings.extentChunks;
	for (int axis = 0; axis < 2; ++axis)
	{
		auto d = a.chunk[axis] - b.chunk[axis];
		if (d > extent / 2)
			d -= extent;
		else if (d < -extent / 2)
			d += extent;
		a.chunk[axis] = d;
		b.chunk[axis] = 0;
	}
	return Delta (a, b, result, limit);
}

bool ApplyWorldDescriptor (WorldConfig &config, const std::vector<uint8_t> &bytes, std::string *error)
{
	auto fail = [&]
	{
		if (error)
			*error = "unsupported generator descriptor";
		return false;
	};
	if (bytes.size () < 52 || bytes[0] != 'Q' || bytes[1] != 'W' || bytes[2] != 'G' || bytes[3] != 1)
		return fail ();
	auto read = [&] (size_t offset)
	{
		uint64_t v = 0;
		for (int i = 0; i < 8; ++i)
			v |= uint64_t (bytes[offset + i]) << (i * 8);
		return v;
	};
	if (read (4) != 1 || read (12) > 2 || read (28) != ChunkSize || read (36) != VoxelSize || read (44) != 1)
		return fail ();
	WorldConfig candidate = config;
	candidate.generator = static_cast<Generator> (read (12));
	candidate.seed = read (20);
	candidate.planet.reset ();
	if (candidate.generator == Generator::Planet)
	{
		if (!DecodePlanetSettings (std::span<const uint8_t> (bytes).subspan (52), candidate.planetSettings, error))
			return false;
	}
	else if (bytes.size () != 52)
		return fail ();
	config = std::move (candidate);
	return true;
}

int TerrainHeightVoxels (uint64_t seed, int64_t chunkX, int64_t chunkY, int voxelX, int voxelY)
{
	if (voxelX < 0 || voxelX > VoxelsPerChunk || voxelY < 0 || voxelY > VoxelsPerChunk)
		return 0;
	uint64_t x = static_cast<uint64_t> (chunkX) * TerrainGrid + voxelX / TerrainSpacing;
	uint64_t y = static_cast<uint64_t> (chunkY) * TerrainGrid + voxelY / TerrainSpacing;
	return TerrainInterpolatedHeight (
		chunkX, chunkY, voxelX, voxelY, TerrainLatticeValue (seed, x, y), TerrainLatticeValue (seed, x + 1, y), TerrainLatticeValue (seed, x, y + 1),
		TerrainLatticeValue (seed, x + 1, y + 1));
}

float TerrainSurfaceHeight (uint64_t seed, int64_t chunkX, int64_t chunkY, float localX, float localY)
{
	if (!std::isfinite (localX) || !std::isfinite (localY) || localX < 0 || localX > ChunkSize || localY < 0 || localY > ChunkSize)
		return 0;
	return float (TerrainContinuousHeight (seed, chunkX, chunkY, localX, localY));
}

std::array<float, 3> TerrainSurfaceNormal (uint64_t seed, int64_t chunkX, int64_t chunkY, float localX, float localY)
{
	if (!std::isfinite (localX) || !std::isfinite (localY) || localX < 0 || localX > ChunkSize || localY < 0 || localY > ChunkSize)
		return {0, 0, 1};
	// Centered samples soften the changes of slope at the generator's lattice
	// and spawn blend boundaries without quantized voxel steps affecting light.
	constexpr double Radius = 32;
	const double	 dx =
		(TerrainContinuousHeight (seed, chunkX, chunkY, localX + Radius, localY) - TerrainContinuousHeight (seed, chunkX, chunkY, localX - Radius, localY)) /
		(2 * Radius);
	const double dy =
		(TerrainContinuousHeight (seed, chunkX, chunkY, localX, localY + Radius) - TerrainContinuousHeight (seed, chunkX, chunkY, localX, localY - Radius)) /
		(2 * Radius);
	const double inverseLength = 1 / std::sqrt (1 + dx * dx + dy * dy);
	return {float (-dx * inverseLength), float (-dy * inverseLength), float (inverseLength)};
}

int64_t &ChunkKey::operator[] (size_t axis)
{
	assert (axis < 3);
	return axis == 0 ? x : axis == 1 ? y : z;
}
int64_t ChunkKey::operator[] (size_t axis) const
{
	assert (axis < 3);
	return axis == 0 ? x : axis == 1 ? y : z;
}

bool Normalize (WorldPosition &position)
{
	return Canonicalize (position, {}, position);
}
bool Add (const WorldPosition &position, const std::array<double, 3> &offset, WorldPosition &result)
{
	return Canonicalize (position, offset, result);
}
bool Delta (const WorldPosition &to, const WorldPosition &from, std::array<double, 3> &result, double limit)
{
	if (!std::isfinite (limit) || limit < 0)
		return false;
	std::array<double, 3> candidate;
	for (size_t i = 0; i != 3; ++i)
	{
		// Subtract in integer space before converting: platforms with 64-bit
		// long double would otherwise lose adjacent chunks near INT64_MAX.
		bool		positive = to.chunk[i] >= from.chunk[i];
		uint64_t	magnitude = positive ? static_cast<uint64_t> (to.chunk[i]) - static_cast<uint64_t> (from.chunk[i])
										 : static_cast<uint64_t> (from.chunk[i]) - static_cast<uint64_t> (to.chunk[i]);
		long double d = static_cast<long double> (magnitude) * (positive ? ChunkSize : -ChunkSize) + static_cast<long double> (to.local[i]) - from.local[i];
		if (!std::isfinite (d) || std::abs (d) > limit)
			return false;
		candidate[i] = static_cast<double> (d);
	}
	result = candidate;
	return true;
}

struct Octree::Impl
{
	Node root;
};
Octree::Octree () : impl (std::make_unique<Impl> ()) {}
Octree::~Octree () = default;
Octree::Octree (const Octree &other) : impl (std::make_unique<Impl> (*other.impl)) {}
Octree &Octree::operator= (const Octree &other)
{
	if (this != &other)
		impl = std::make_unique<Impl> (*other.impl);
	return *this;
}
Octree::Octree (Octree &&) noexcept = default;
Octree &Octree::operator= (Octree &&) noexcept = default;
Cell	Octree::Get (int x, int y, int z) const
{
	if (x < 0 || y < 0 || z < 0 || x >= VoxelsPerChunk || y >= VoxelsPerChunk || z >= VoxelsPerChunk)
		return {0, Unknown};
	return impl->root.Get (x, y, z, VoxelsPerChunk);
}
bool Octree::Set (int x, int y, int z, Cell cell)
{
	if (x < 0 || y < 0 || z < 0 || x >= VoxelsPerChunk || y >= VoxelsPerChunk || z >= VoxelsPerChunk || !ValidCell (cell))
		return false;
	return impl->root.Set (x, y, z, VoxelsPerChunk, cell);
}
void Octree::Fill (Cell cell)
{
	if (!ValidCell (cell))
		throw std::invalid_argument ("invalid voxel contents");
	impl->root.Fill (cell);
}
void Octree::Visit (const std::function<void (int, int, int, int, Cell)> &visitor) const
{
	impl->root.Visit (0, 0, 0, VoxelsPerChunk, visitor);
}
size_t Octree::NodeCount () const
{
	return impl->root.Count ();
}
std::vector<uint8_t> Octree::Encode () const
{
	std::vector<uint8_t> result = {'Q', 'O', 'T', 1};
	impl->root.Encode (result);
	return result;
}
bool Octree::Decode (const std::vector<uint8_t> &bytes)
{
	if (bytes.size () < 5 || bytes.size () > MaxEncodedBytes || bytes[0] != 'Q' || bytes[1] != 'O' || bytes[2] != 'T' || bytes[3] != 1)
		return false;
	auto   candidate = std::make_unique<Impl> ();
	size_t cursor = 4;
	if (!candidate->root.Decode (bytes, cursor, 0) || cursor != bytes.size ())
		return false;
	impl = std::move (candidate);
	return true;
}

namespace
{
size_t ChunkResidentBytes (const Chunk &chunk)
{
	size_t bytes = sizeof (Chunk) + chunk.cells.NodeCount () * sizeof (Node);
	if (chunk.solids)
	{
		bytes += sizeof (*chunk.solids) + chunk.solids->capacity () * sizeof (SolidDefinition);
		for (const auto &s : *chunk.solids)
			bytes += s.vertices.capacity () * sizeof (SolidPoint) + s.faces.capacity () * sizeof (SolidFace);
	}
	return bytes;
}
} // namespace
struct World::Impl
{
	struct Entry
	{
		std::shared_ptr<Chunk>	   chunk;
		std::shared_ptr<ChunkMesh> mesh;
		uint64_t				   touched = 0;
		uint64_t				   meshSerial = 0;
		size_t					   treeBytes = 0;
		uint64_t				   failedMeshSerial = 0;
		std::optional<size_t>	   meshFootprint;
	};
	struct MeshJob
	{
		uint64_t								serial;
		std::future<std::shared_ptr<ChunkMesh>> future;
	};
	struct CollisionPatch
	{
		std::shared_ptr<ChunkMesh> mesh;
		uint64_t				   touched = 0;
		std::vector<float>		   density;
		size_t					   Bytes () const
		{
			return mesh->Bytes () + density.capacity () * sizeof (float);
		}
	};
	using CollisionId = std::array<int64_t, 6>;
	struct CollisionJob
	{
		uint64_t									 serial;
		size_t										 inputBytes;
		std::shared_ptr<std::atomic<bool>>			 canceled;
		std::future<std::shared_ptr<CollisionPatch>> future;
	};
	std::map<CollisionId, CollisionJob>				 collisionPending;
	std::map<CollisionId, uint64_t>					 collisionFailures;
	size_t											 collisionPendingBytes = 0;
	uint64_t										 collisionStaleResults = 0;
	// Collision owns a small spatial cache, independent of asynchronous render
	// meshes. A walking trace must never wait behind full-chunk extraction.
	static constexpr int							 CollisionCells = 16;
	static constexpr size_t							 CollisionBudget = 32 * 1024 * 1024;
	static constexpr size_t							 CollisionInputBudget = 256 * 1024 * 1024;
	static constexpr size_t							 CollisionJobLimit = 32;
	static constexpr int							 CollisionSamples = CollisionCells + 2;
	std::map<std::array<int64_t, 6>, CollisionPatch> collisionPatches;
	size_t											 collisionBytes = 0;
	uint64_t										 collisionBuilds = 0;
	uint64_t										 collisionSyncBuilds = 0, collisionWorkerBuilds = 0;
	struct EditJob
	{
		std::shared_ptr<Chunk> chunk;
		std::future<void>	   future;
		World::EditCompletion  completion;
		size_t				   bytes;
		uint64_t			   previousRevision;
	};
	struct PageJob
	{
		std::future<void>						  future;
		std::function<void (const std::string &)> completion;
		size_t									  bytes;
		uint64_t								  writeId;
	};
	std::deque<PageJob>														   pageJobs;
	std::unordered_set<uint64_t>											   pageWrites;
	size_t																	   pendingPageBytes = 0;
	std::unordered_map<ChunkKey, EditJob, KeyHash>							   edits;
	size_t																	   pendingEditBytes = 0;
	std::deque<std::function<void ()>>										   priorityJobs;
	WorldConfig																   config;
	std::unordered_map<ChunkKey, Entry, KeyHash>							   resident;
	std::unordered_map<ChunkKey, std::future<std::shared_ptr<Chunk>>, KeyHash> pending;
	std::unordered_map<ChunkKey, MeshJob, KeyHash>							   meshes;
	std::unordered_set<ChunkKey, KeyHash>									   pinned;
	uint64_t																   clock = 0;
	uint64_t																   serial = 0;
	std::string																   error;
	sqlite3																	  *db = nullptr;
	std::string																   migrationDestination;
	std::mutex																   contentMutex;
	ContentHash																   cachedRoot{};
	uint64_t																   cachedEpoch = 1;
	std::mutex																   jobMutex;
	std::condition_variable													   jobCondition;
	std::condition_variable													   meshCondition;
	std::deque<std::function<void ()>>										   meshJobs;
	std::deque<std::function<void ()>>										   jobs;
	bool																	   stopping = false;
	std::thread																   worker;
	std::thread																   meshWorker;
	std::thread																   collisionWorker;
	std::condition_variable													   collisionCondition;
	std::deque<std::function<void ()>>										   collisionJobs;
	// Guarded by jobMutex. Completed worker products remain bounded even if
	// the owner pauses publication; one currently built product is additional.
	size_t																	   collisionResultBytes = 0;

	template <typename Function> auto SubmitMesh (Function function)
	{
		using Result = std::invoke_result_t<Function>;
		auto task = std::make_shared<std::packaged_task<Result ()>> (std::move (function));
		auto future = task->get_future ();
		{
			std::lock_guard lock (jobMutex);
			meshJobs.emplace_back ([task] { (*task) (); });
		}
		meshCondition.notify_one ();
		return future;
	}

	template <typename Function> auto Submit (Function function, bool priority = false)
	{
		using Result = std::invoke_result_t<Function>;
		auto task = std::make_shared<std::packaged_task<Result ()>> (std::move (function));
		auto future = task->get_future ();
		{
			std::lock_guard lock (jobMutex);
			if (priority)
				priorityJobs.emplace_back ([task] { (*task) (); });
			else
				jobs.emplace_back ([task] { (*task) (); });
		}
		jobCondition.notify_one ();
		return future;
	}
	bool PageFailure (const std::string &message, std::string *output)
	{
		error = message;
		if (output)
			*output = error;
		return false;
	}
	bool QueuePageJob (uint64_t writeId, size_t bytes, std::function<void ()> work, std::function<void (const std::string &)> completion, std::string *output)
	{
		if (writeId && pageWrites.contains (writeId))
			return PageFailure ("entity page has a pending write", output);
		if (pageJobs.size () >= std::min (config.pendingLimit, size_t (64)) || bytes > 64 * MaxEntityPageBytes - pendingPageBytes)
			return PageFailure ("entity page queue or memory budget exceeded", output);
		pageJobs.push_back ({Submit (std::move (work), writeId != 0), std::move (completion), bytes, writeId});
		if (writeId)
			pageWrites.insert (writeId);
		pendingPageBytes += bytes;
		return true;
	}

	std::deque<std::function<void ()>> surfaceCompletions;
	std::atomic<uint64_t>			   generationProgress{0};
	explicit Impl (WorldConfig value) : config (std::move (value))
	{
		if (config.residentLimit < 1 || config.residentLimit > 65536 || config.pendingLimit < 1 || config.pendingLimit > 4096 ||
			config.residentBytes < sizeof (Chunk) + sizeof (Node) || config.meshBytes < sizeof (MeshQuad))
			throw std::invalid_argument ("invalid world cache limits");
		worker = std::thread (
			[this]
			{
				for (;;)
				{
					std::function<void ()> job;
					{
						std::unique_lock lock (jobMutex);
						jobCondition.wait (lock, [this] { return stopping || !priorityJobs.empty () || !jobs.empty (); });
						if (priorityJobs.empty () && jobs.empty () && stopping)
							return;
						auto &queue = priorityJobs.empty () ? jobs : priorityJobs;
						job = std::move (queue.front ());
						queue.pop_front ();
					}
					job ();
				}
			});
		try
		{
			meshWorker = std::thread (
				[this]
				{
					for (;;)
					{
						std::function<void ()> job;
						{
							std::unique_lock lock (jobMutex);
							meshCondition.wait (lock, [this] { return stopping || !meshJobs.empty (); });
							if (meshJobs.empty () && stopping)
								return;
							job = std::move (meshJobs.front ());
							meshJobs.pop_front ();
						}
						job ();
					}
				});
			collisionWorker = std::thread (
				[this]
				{
					for (;;)
					{
						std::function<void ()> job;
						{
							std::unique_lock lock (jobMutex);
							collisionCondition.wait (lock, [this] { return stopping || !collisionJobs.empty (); });
							if (collisionJobs.empty () && stopping)
								return;
							job = std::move (collisionJobs.front ());
							collisionJobs.pop_front ();
						}
						job ();
					}
				});
			auto opened = Submit (
				[this]
				{
					Open ();
					FinishMigration ();
				});
			while (opened.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready)
			{
				const auto progress = generationProgress.load ();
				if (config.generationProgress)
					config.generationProgress (uint32_t (progress), uint32_t (progress >> 32));
			}
			opened.get ();
		}
		catch (...)
		{
			Stop ();
			throw;
		}
	}

	void Stop ()
	{
		for (auto &[id, job] : collisionPending)
			job.canceled->store (true);
		{
			std::lock_guard lock (jobMutex);
			stopping = true;
		}
		jobCondition.notify_one ();
		meshCondition.notify_one ();
		collisionCondition.notify_all ();
		if (collisionWorker.joinable ())
			collisionWorker.join ();
		if (meshWorker.joinable ())
			meshWorker.join ();
		if (worker.joinable ())
			worker.join ();
		if (db)
			sqlite3_close (db);
		db = nullptr;
	}
	~Impl ()
	{
		Stop ();
	}

	void SelectMigrationDatabase ()
	{
		if (config.databasePath.empty () || !std::filesystem::exists (config.databasePath))
			return;
		sqlite3 *original = nullptr;
		SqlCheck (sqlite3_open_v2 (config.databasePath.c_str (), &original, SQLITE_OPEN_READONLY, nullptr), original, "inspect legacy database");
		try
		{
			bool legacy = false;
			{
				Statement tables (
					original,
					"SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE name='world_metadata'), EXISTS(SELECT 1 FROM sqlite_master WHERE name='terrain_state')");
				SqlCheck (sqlite3_step (tables.statement), original, "inspect storage format");
				legacy = sqlite3_column_int (tables.statement, 0) && !sqlite3_column_int (tables.statement, 1);
			}
			if (legacy)
			{
				const std::string destinationPath = config.databasePath + ".merkle";
				if (!std::filesystem::exists (destinationPath))
				{
					const std::string temporary = destinationPath + ".tmp";
					sqlite3			 *destination = nullptr;
					SqlCheck (
						sqlite3_open_v2 (temporary.c_str (), &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), destination,
						"open migration copy");
					try
					{
						sqlite3_backup *backup = sqlite3_backup_init (destination, "main", original, "main");
						if (!backup)
							throw std::runtime_error (sqlite3_errmsg (destination));
						int status = sqlite3_backup_step (backup, -1), finish = sqlite3_backup_finish (backup);
						SqlCheck (status, destination, "copy legacy world");
						SqlCheck (finish, destination, "finish migration copy");
						Statement check (destination, "PRAGMA integrity_check");
						if (sqlite3_step (check.statement) != SQLITE_ROW ||
							std::string (reinterpret_cast<const char *> (sqlite3_column_text (check.statement, 0))) != "ok")
							throw std::runtime_error ("migration copy failed integrity check");
					}
					catch (...)
					{
						sqlite3_close (destination);
						throw;
					}
					sqlite3_close (destination);
					migrationDestination = destinationPath;
					config.databasePath = temporary;
				}
				else
					config.databasePath = destinationPath;
			}
		}
		catch (...)
		{
			sqlite3_close (original);
			throw;
		}
		sqlite3_close (original);
	}

	void FinishMigration ()
	{
		if (migrationDestination.empty ())
			return;
		SqlCheck (sqlite3_wal_checkpoint_v2 (db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr), db, "checkpoint converted world");
		SqlCheck (sqlite3_close (db), db, "close converted world");
		db = nullptr;
		std::filesystem::rename (config.databasePath, migrationDestination);
		config.databasePath = std::move (migrationDestination);
		migrationDestination.clear ();
		Open ();
	}

	void Open ()
	{
		SelectMigrationDatabase ();
		// SQLite's empty filename creates a temporary disk-backed database, keeping
		// edits alive after cache eviction without an unbounded in-memory overlay.
		SqlCheck (
			sqlite3_open_v2 (config.databasePath.c_str (), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr), db, "open world");
		sqlite3_busy_timeout (db, 5000);
		if (!config.databasePath.empty ())
		{
			Statement journal (db, "PRAGMA journal_mode=WAL");
			SqlCheck (sqlite3_step (journal.statement), db, "enable WAL");
			const char *mode = reinterpret_cast<const char *> (sqlite3_column_text (journal.statement, 0));
			if (!mode || std::string (mode) != "wal")
				throw std::runtime_error ("world database does not support WAL");
		}
		SqlExec (db, "PRAGMA synchronous=FULL");
		SqlExec (db, "PRAGMA cache_size=-4096");
		SqlExec (
			db, "CREATE TABLE IF NOT EXISTS world_metadata (id INTEGER PRIMARY KEY CHECK(id=1), format INTEGER NOT NULL, generator INTEGER NOT NULL, seed TEXT "
				"NOT NULL)");
		SqlExec (
			db, "CREATE TABLE IF NOT EXISTS chunks (x INTEGER NOT NULL,y INTEGER NOT NULL,z INTEGER NOT NULL,revision INTEGER NOT NULL,data BLOB NOT "
				"NULL,PRIMARY KEY(x,y,z)) WITHOUT ROWID");
		SqlExec (
			db, "CREATE TABLE IF NOT EXISTS world_checkpoint (id INTEGER PRIMARY KEY CHECK(id=1),format INTEGER NOT NULL,bytes INTEGER NOT NULL,checksum "
				"INTEGER NOT NULL,data BLOB NOT NULL)");
		SqlExec (db, "CREATE TABLE IF NOT EXISTS entity_page_ids (id BLOB PRIMARY KEY,last_revision INTEGER NOT NULL) WITHOUT ROWID");
		SqlExec (
			db, "CREATE TABLE IF NOT EXISTS entity_page_versions (id BLOB NOT NULL,revision INTEGER NOT NULL,format INTEGER NOT NULL,"
				"x INTEGER NOT NULL,y INTEGER NOT NULL,z INTEGER NOT NULL,bytes INTEGER NOT NULL,checksum INTEGER NOT NULL,deleted INTEGER NOT NULL,"
				"data BLOB NOT NULL,PRIMARY KEY(id,revision)) WITHOUT ROWID");
		SqlExec (db, "CREATE TABLE IF NOT EXISTS entity_page_heads (id BLOB PRIMARY KEY,revision INTEGER NOT NULL) WITHOUT ROWID");
		SqlExec (db, "CREATE TABLE IF NOT EXISTS entity_page_checkpoint (id BLOB PRIMARY KEY,revision INTEGER NOT NULL) WITHOUT ROWID");
		SqlExec (db, "CREATE INDEX IF NOT EXISTS entity_page_spatial ON entity_page_versions(x,y,z,id)");
		SqlExec (db, "CREATE TABLE IF NOT EXISTS entity_page_sequence (id INTEGER PRIMARY KEY CHECK(id=1),last_id BLOB NOT NULL)");
		SqlExec (db, "INSERT OR IGNORE INTO entity_page_sequence VALUES(1,X'0000000000000000')");
		Statement metadata (db, "SELECT format,generator,seed FROM world_metadata WHERE id=1");
		int		  status = sqlite3_step (metadata.statement);
		SqlCheck (status, db, "read world metadata");
		if (status == SQLITE_ROW)
		{
			int			version = sqlite3_column_int (metadata.statement, 0);
			int			generator = sqlite3_column_int (metadata.statement, 1);
			const char *seed = reinterpret_cast<const char *> (sqlite3_column_text (metadata.statement, 2));
			if (version != 1 || generator < 0 || generator > 2 || !seed)
				throw std::runtime_error ("unsupported world database format or generator");
			config.generator = static_cast<Generator> (generator);
			size_t used = 0;
			config.seed = std::stoull (seed, &used, 10);
			if (used != std::string (seed).size ())
				throw std::runtime_error ("invalid world seed");
		}
		else
		{
			Statement insert (db, "INSERT INTO world_metadata VALUES(1,1,?,?)");
			SqlCheck (sqlite3_bind_int (insert.statement, 1, static_cast<int> (config.generator)), db, "bind generator");
			std::string seed = std::to_string (config.seed);
			SqlCheck (sqlite3_bind_text (insert.statement, 2, seed.c_str (), -1, SQLITE_TRANSIENT), db, "bind seed");
			SqlCheck (sqlite3_step (insert.statement), db, "save world metadata");
		}

		if (config.generator == Generator::Planet)
		{
			SqlExec (db, "CREATE TABLE IF NOT EXISTS planet_settings(id INTEGER PRIMARY KEY CHECK(id=1),data BLOB NOT NULL)");
			Statement settings (db, "SELECT data FROM planet_settings WHERE id=1");
			int		  status = sqlite3_step (settings.statement);
			SqlCheck (status, db, "read planet settings");
			if (status == SQLITE_ROW)
			{
				auto data = static_cast<const uint8_t *> (sqlite3_column_blob (settings.statement, 0));
				if (!DecodePlanetSettings (
						std::span<const uint8_t> (data, size_t (sqlite3_column_bytes (settings.statement, 0))), config.planetSettings, &error))
					throw std::runtime_error (error);
			}
			else
			{
				if (sqlite3_column_count (metadata.statement) && sqlite3_data_count (metadata.statement))
					throw std::runtime_error ("missing planet settings in existing world");
				auto data = EncodePlanetSettings (config.planetSettings);
				if (data.empty ())
					throw std::runtime_error ("invalid planet settings");
				Statement insert (db, "INSERT INTO planet_settings VALUES(1,?)");
				SqlCheck (sqlite3_bind_blob (insert.statement, 1, data.data (), int (data.size ()), SQLITE_TRANSIENT), db, "bind planet settings");
				SqlCheck (sqlite3_step (insert.statement), db, "save planet settings");
			}
			if (!config.planet || config.planet->Settings () != config.planetSettings || config.planet->Seed () != config.seed)
				config.planet = std::make_shared<PlanetGenerator> (config.planetSettings, config.seed);
			if (!config.planet->Ready ())
			{
				const std::string cache = config.databasePath.empty () ? std::string{} : config.databasePath + ".planetcache";
				if (cache.empty () || !config.planet->LoadCache (cache))
				{
					if (!config.planet->Prepare (&error, [this] (uint32_t n, uint32_t total) { generationProgress.store ((uint64_t (total) << 32) | n); }))
						throw std::runtime_error (error);
					if (!cache.empty ())
						config.planet->SaveCache (cache);
				}
			}
		}
		OpenMerkle ();
		// Opening a migrated copy succeeds only after its converted schema and data verify.
		if (!migrationDestination.empty ())
		{
			Statement check (db, "PRAGMA integrity_check");
			if (sqlite3_step (check.statement) != SQLITE_ROW || std::string (reinterpret_cast<const char *> (sqlite3_column_text (check.statement, 0))) != "ok")
				throw std::runtime_error ("converted world failed integrity check");
		}
		RefreshContentHead ();
	}

	std::shared_ptr<Chunk> Generate (ChunkKey key)
	{
		auto chunk = std::make_shared<Chunk> ();
		chunk->key = key;

		if (config.generator == Generator::Planet)
		{
			int heights[VoxelsPerChunk][VoxelsPerChunk];
			for (int x = 0; x < VoxelsPerChunk; ++x)
				for (int y = 0; y < VoxelsPerChunk; ++y)
				{
					const double h =
						config.planet->Height (key.x, key.y, x * VoxelSize + VoxelSize / 2, y * VoxelSize + VoxelSize / 2) - double (key.z) * ChunkSize;
					heights[x][y] = int (std::clamp (std::ceil ((h - VoxelSize / 2) / VoxelSize), 0.0, double (VoxelsPerChunk)));
				}
			const int waterHeight = int (std::clamp (
				std::ceil ((double (config.planetSettings.seaLevel) - double (key.z) * ChunkSize - VoxelSize / 2) / VoxelSize), 0.0, double (VoxelsPerChunk)));
			auto	  build = [&] (auto &&self, Node &node, int x, int y, int z, int size) -> void
			{
				int low = VoxelsPerChunk, high = 0;
				for (int a = x; a < x + size; ++a)
					for (int b = y; b < y + size; ++b)
					{
						low = std::min (low, heights[a][b]);
						high = std::max (high, heights[a][b]);
					}
				if (z >= high && z >= waterHeight)
					node.Fill ({});
				else if (z >= high && z + size <= waterHeight)
					node.Fill ({5, Water});
				else if (z + size <= low)
					node.Fill ({1, Solid});
				else
				{
					node.Split ();
					int half = size / 2;
					for (int i = 0; i < 8; ++i)
						self (self, (*node.children)[i], x + (i & 1) * half, y + ((i >> 1) & 1) * half, z + ((i >> 2) & 1) * half, half);
					node.Collapse ();
				}
			};
			build (build, chunk->cells.impl->root, 0, 0, 0, VoxelsPerChunk);
			return chunk;
		}
		if (config.generator == Generator::Empty || key.z > 0)
			return chunk;
		if (key.z < 0)
		{
			chunk->cells.Fill ({1, Solid});
			return chunk;
		}
		// Reuse the sampler's exact lattice/interpolation while retaining the
		// original 25-hash chunk cache instead of rehashing every voxel column.
		int lattice[TerrainGrid + 1][TerrainGrid + 1];
		for (int x = 0; x <= TerrainGrid; ++x)
			for (int y = 0; y <= TerrainGrid; ++y)
				lattice[x][y] =
					TerrainLatticeValue (config.seed, static_cast<uint64_t> (key.x) * TerrainGrid + x, static_cast<uint64_t> (key.y) * TerrainGrid + y);
		int heights[VoxelsPerChunk][VoxelsPerChunk];
		for (int x = 0; x < VoxelsPerChunk; ++x)
			for (int y = 0; y < VoxelsPerChunk; ++y)
			{
				const int bx = x / TerrainSpacing, by = y / TerrainSpacing;
				heights[x][y] =
					TerrainInterpolatedHeight (key.x, key.y, x, y, lattice[bx][by], lattice[bx + 1][by], lattice[bx][by + 1], lattice[bx + 1][by + 1]);
			}
		std::function<void (Node &, int, int, int, int)> build = [&] (Node &node, int x, int y, int z, int size)
		{
			int minimum = 128, maximum = 0;
			for (int bx = x; bx < x + size; ++bx)
				for (int by = y; by < y + size; ++by)
				{
					minimum = std::min (minimum, heights[bx][by]);
					maximum = std::max (maximum, heights[bx][by]);
				}
			if (z >= maximum)
				node.Fill ({});
			else if (z + size <= minimum)
				node.Fill ({static_cast<uint16_t> (z + size == maximum ? 2 : 1), Solid});
			else
			{
				assert (size > 1);
				node.Split ();
				int half = size / 2;
				for (int i = 0; i < 8; ++i)
					build ((*node.children)[i], x + (i & 1) * half, y + ((i >> 1) & 1) * half, z + ((i >> 2) & 1) * half, half);
				node.Collapse ();
			}
		};
		build (chunk->cells.impl->root, 0, 0, 0, VoxelsPerChunk);
		return chunk;
	}

#include "world_merkle_store.inc"

	void PrunePages ()
	{
		SqlExec (
			db, "DELETE FROM entity_page_versions AS v WHERE NOT EXISTS(SELECT 1 FROM entity_page_heads h WHERE h.id=v.id AND h.revision=v.revision) "
				"AND NOT EXISTS(SELECT 1 FROM entity_page_checkpoint c WHERE c.id=v.id AND c.revision=v.revision)");
	}

	uint64_t AllocatePageId ()
	{
		SqlExec (db, "BEGIN IMMEDIATE");
		try
		{
			uint64_t id;
			{
				Statement current (db, "SELECT last_id FROM entity_page_sequence WHERE id=1");
				if (sqlite3_step (current.statement) != SQLITE_ROW)
					throw std::runtime_error ("entity page ID sequence is missing");
				id = ReadPageId (current.statement, 0);
			}
			if (id == UINT64_MAX)
				throw std::runtime_error ("entity page IDs exhausted");
			++id;
			Statement update (db, "UPDATE entity_page_sequence SET last_id=? WHERE id=1");
			update.Id (1, id);
			SqlCheck (sqlite3_step (update.statement), db, "allocate entity page ID");
			Statement reserve (db, "INSERT INTO entity_page_ids(id,last_revision) VALUES(?,0)");
			reserve.Id (1, id);
			SqlCheck (sqlite3_step (reserve.statement), db, "reserve entity page ID");
			SqlExec (db, "COMMIT");
			return id;
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	uint64_t StorePage (uint64_t id, ChunkKey key, const std::vector<uint8_t> &bytes, uint64_t expected, bool deleted)
	{
		SqlExec (db, "BEGIN IMMEDIATE");
		try
		{
			uint64_t current = 0, last = 0;
			{
				Statement head (db, "SELECT revision FROM entity_page_heads WHERE id=?");
				head.Id (1, id);
				int status = sqlite3_step (head.statement);
				SqlCheck (status, db, "read entity page revision");
				if (status == SQLITE_ROW)
				{
					if (sqlite3_column_type (head.statement, 0) != SQLITE_INTEGER || sqlite3_column_int64 (head.statement, 0) <= 0)
						throw std::runtime_error ("invalid entity page revision");
					current = uint64_t (sqlite3_column_int64 (head.statement, 0));
				}
				Statement clock (db, "SELECT last_revision FROM entity_page_ids WHERE id=?");
				clock.Id (1, id);
				status = sqlite3_step (clock.statement);
				SqlCheck (status, db, "read entity page revision clock");
				if (status == SQLITE_ROW)
				{
					if (sqlite3_column_type (clock.statement, 0) != SQLITE_INTEGER || sqlite3_column_int64 (clock.statement, 0) < 0)
						throw std::runtime_error ("invalid entity page revision clock");
					last = uint64_t (sqlite3_column_int64 (clock.statement, 0));
				}
			}
			if (current != expected || (deleted && !current))
				throw std::runtime_error ("entity page revision conflict");
			if (last < current || last == uint64_t (INT64_MAX))
				throw std::runtime_error ("entity page revision clock exhausted or corrupt");
			if (deleted)
			{
				Statement previous (db, "SELECT x,y,z FROM entity_page_versions WHERE id=? AND revision=?");
				previous.Id (1, id);
				SqlCheck (sqlite3_bind_int64 (previous.statement, 2, int64_t (current)), db, "bind previous revision");
				if (sqlite3_step (previous.statement) != SQLITE_ROW)
					throw std::runtime_error ("entity page head has no data");
				for (int axis = 0; axis < 3; ++axis)
				{
					if (sqlite3_column_type (previous.statement, axis) != SQLITE_INTEGER)
						throw std::runtime_error ("invalid entity page chunk coordinate");
					key[axis] = sqlite3_column_int64 (previous.statement, axis);
				}
			}
			const uint64_t revision = last + 1;
			Statement	   version (db, "INSERT INTO entity_page_versions(id,revision,format,x,y,z,bytes,checksum,deleted,data) VALUES(?,?,1,?,?,?,?,?,?,?)");
			version.Id (1, id);
			SqlCheck (sqlite3_bind_int64 (version.statement, 2, int64_t (revision)), db, "bind page revision");
			for (int axis = 0; axis < 3; ++axis)
				SqlCheck (sqlite3_bind_int64 (version.statement, 3 + axis, key[axis]), db, "bind page chunk");
			SqlCheck (sqlite3_bind_int64 (version.statement, 6, int64_t (bytes.size ())), db, "bind page length");
			SqlCheck (sqlite3_bind_int64 (version.statement, 7, CheckpointChecksum (bytes)), db, "bind page checksum");
			SqlCheck (sqlite3_bind_int (version.statement, 8, deleted), db, "bind page deletion");
			if (bytes.empty ())
				SqlCheck (sqlite3_bind_zeroblob (version.statement, 9, 0), db, "bind empty page");
			else
				SqlCheck (sqlite3_bind_blob (version.statement, 9, bytes.data (), int (bytes.size ()), SQLITE_TRANSIENT), db, "bind page data");
			SqlCheck (sqlite3_step (version.statement), db, "save entity page");
			for (const char *sql : {"INSERT OR REPLACE INTO entity_page_heads VALUES(?,?)", "INSERT OR REPLACE INTO entity_page_ids VALUES(?,?)"})
			{
				Statement update (db, sql);
				update.Id (1, id);
				SqlCheck (sqlite3_bind_int64 (update.statement, 2, int64_t (revision)), db, "bind page head");
				SqlCheck (sqlite3_step (update.statement), db, "publish entity page head");
			}
			Statement sequence (db, "UPDATE entity_page_sequence SET last_id=?1 WHERE id=1 AND last_id<?1");
			sequence.Id (1, id);
			SqlCheck (sqlite3_step (sequence.statement), db, "advance page ID sequence");
			Statement prune (
				db, "DELETE FROM entity_page_versions WHERE id=?1 AND revision<>?2 AND revision NOT IN "
					"(SELECT revision FROM entity_page_checkpoint WHERE id=?1)");
			prune.Id (1, id);
			SqlCheck (sqlite3_bind_int64 (prune.statement, 2, int64_t (revision)), db, "bind retained page version");
			SqlCheck (sqlite3_step (prune.statement), db, "prune previous entity page");
			SqlExec (db, "COMMIT");
			return revision;
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	std::optional<EntityPage> ReadPage (uint64_t id)
	{
		SqlExec (db, "BEGIN");
		try
		{
			EntityPage page;
			page.info.id = id;
			int64_t	 length = 0;
			uint32_t checksum = 0;
			bool	 present = false;
			{
				Statement header (
					db, "SELECT v.revision,v.format,v.x,v.y,v.z,v.bytes,v.checksum,v.deleted,length(v.data),typeof(v.data) "
						"FROM entity_page_heads h LEFT JOIN entity_page_versions v ON h.id=v.id AND h.revision=v.revision WHERE h.id=?");
				header.Id (1, id);
				int status = sqlite3_step (header.statement);
				SqlCheck (status, db, "read entity page metadata");
				if (status == SQLITE_ROW)
				{
					for (int column = 0; column < 9; ++column)
						if (sqlite3_column_type (header.statement, column) != SQLITE_INTEGER)
							throw std::runtime_error ("invalid entity page metadata type");
					const int64_t revision = sqlite3_column_int64 (header.statement, 0);
					const int64_t format = sqlite3_column_int64 (header.statement, 1);
					length = sqlite3_column_int64 (header.statement, 5);
					const int64_t encodedChecksum = sqlite3_column_int64 (header.statement, 6);
					const int64_t deleted = sqlite3_column_int64 (header.statement, 7);
					const int64_t actualLength = sqlite3_column_int64 (header.statement, 8);
					const char	 *type = reinterpret_cast<const char *> (sqlite3_column_text (header.statement, 9));
					if (format != 1 || revision <= 0 || length < 0 || uint64_t (length) > MaxEntityPageBytes || actualLength != length || encodedChecksum < 0 ||
						uint64_t (encodedChecksum) > UINT32_MAX || (deleted != 0 && deleted != 1) || !type || std::string (type) != "blob" ||
						(deleted && (length || encodedChecksum)))
						throw std::runtime_error ("invalid entity page metadata");
					page.info.revision = uint64_t (revision);
					for (int axis = 0; axis < 3; ++axis)
						page.info.key[axis] = sqlite3_column_int64 (header.statement, axis + 2);
					checksum = uint32_t (encodedChecksum);
					present = !deleted;
				}
			}
			if (present)
			{
				Statement payload (db, "SELECT data FROM entity_page_versions WHERE id=? AND revision=?");
				payload.Id (1, id);
				SqlCheck (sqlite3_bind_int64 (payload.statement, 2, int64_t (page.info.revision)), db, "bind loaded page revision");
				int status = sqlite3_step (payload.statement);
				SqlCheck (status, db, "read entity page data");
				if (status != SQLITE_ROW || sqlite3_column_type (payload.statement, 0) != SQLITE_BLOB || sqlite3_column_bytes (payload.statement, 0) != length)
					throw std::runtime_error ("entity page changed during read");
				const auto *data = static_cast<const uint8_t *> (sqlite3_column_blob (payload.statement, 0));
				if (length && !data)
					throw std::runtime_error ("cannot read entity page data");
				if (length)
					page.bytes.assign (data, data + length);
				if (CheckpointChecksum (page.bytes) != checksum)
					throw std::runtime_error ("entity page checksum mismatch");
			}
			SqlExec (db, "COMMIT");
			return present ? std::optional<EntityPage> (std::move (page)) : std::nullopt;
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	std::vector<EntityPageInfo> ListPages (ChunkKey minimum, ChunkKey maximum, size_t limit, uint64_t afterId)
	{
		std::string predicate;
		for (int axis = 0; axis < 3; ++axis)
		{
			bool wrap = false, all = false;
			if (config.generator == Generator::Planet && axis < 2)
			{
				all = double (maximum[axis]) - double (minimum[axis]) >= double (config.planetSettings.extentChunks - 1);
				minimum[axis] = CanonicalPlanetChunk (minimum[axis], config.planetSettings.extentChunks);
				maximum[axis] = CanonicalPlanetChunk (maximum[axis], config.planetSettings.extentChunks);
				wrap = minimum[axis] > maximum[axis];
			}
			const std::string col = std::string ("v.") + char ('x' + axis);
			predicate += " AND (" + col + ">=?" + std::to_string (axis * 2 + 1) + (wrap ? " OR " : " AND ") + col + "<=?" + std::to_string (axis * 2 + 2) +
						 (all ? " OR 1)" : ")");
		}
		const std::string query =
			"SELECT h.id,h.revision,v.x,v.y,v.z FROM entity_page_heads h JOIN entity_page_versions v ON h.id=v.id AND h.revision=v.revision WHERE v.deleted=0" +
			predicate + " AND h.id>?7 ORDER BY h.id LIMIT ?8";
		Statement list (db, query.c_str ());
		for (int axis = 0; axis < 3; ++axis)
		{
			SqlCheck (sqlite3_bind_int64 (list.statement, axis * 2 + 1, minimum[axis]), db, "bind page query minimum");
			SqlCheck (sqlite3_bind_int64 (list.statement, axis * 2 + 2, maximum[axis]), db, "bind page query maximum");
		}
		list.Id (7, afterId);
		SqlCheck (sqlite3_bind_int (list.statement, 8, int (limit)), db, "bind page query limit");
		std::vector<EntityPageInfo> result;
		for (int status; (status = sqlite3_step (list.statement)) != SQLITE_DONE;)
		{
			SqlCheck (status, db, "list entity pages");
			for (int column = 1; column < 5; ++column)
				if (sqlite3_column_type (list.statement, column) != SQLITE_INTEGER)
					throw std::runtime_error ("invalid entity page index metadata");
			EntityPageInfo info;
			info.id = ReadPageId (list.statement, 0);
			int64_t revision = sqlite3_column_int64 (list.statement, 1);
			if (!info.id || revision <= 0)
				throw std::runtime_error ("invalid entity page index ID/revision");
			info.revision = uint64_t (revision);
			for (int axis = 0; axis < 3; ++axis)
				info.key[axis] = sqlite3_column_int64 (list.statement, axis + 2);
			result.push_back (info);
		}
		return result;
	}

	void RestorePages ()
	{
		SqlExec (db, "BEGIN IMMEDIATE");
		try
		{
			SqlExec (db, "DELETE FROM entity_page_heads");
			SqlExec (db, "INSERT INTO entity_page_heads SELECT id,revision FROM entity_page_checkpoint");
			PrunePages ();
			SqlExec (db, "COMMIT");
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	void StoreCheckpoint (const std::vector<uint8_t> &bytes)
	{
		const uint32_t checksum = CheckpointChecksum (bytes);
		SqlExec (db, "BEGIN IMMEDIATE");
		try
		{
			Statement statement (db, "INSERT OR REPLACE INTO world_checkpoint(id,format,bytes,checksum,data) VALUES(1,1,?,?,?)");
			SqlCheck (sqlite3_bind_int64 (statement.statement, 1, static_cast<int64_t> (bytes.size ())), db, "bind checkpoint length");
			SqlCheck (sqlite3_bind_int64 (statement.statement, 2, checksum), db, "bind checkpoint checksum");
			if (bytes.empty ())
				SqlCheck (sqlite3_bind_zeroblob (statement.statement, 3, 0), db, "bind empty checkpoint");
			else
				SqlCheck (sqlite3_bind_blob (statement.statement, 3, bytes.data (), static_cast<int> (bytes.size ()), SQLITE_TRANSIENT), db, "bind checkpoint");
			SqlCheck (sqlite3_step (statement.statement), db, "save world checkpoint");
			SqlExec (db, "DELETE FROM entity_page_checkpoint");
			SqlExec (db, "INSERT INTO entity_page_checkpoint SELECT id,revision FROM entity_page_heads");
			PinSnapshot ("latest", bytes);
			GarbageCollect (false);
			SqlExec (db, "COMMIT");
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	std::vector<uint8_t> ReadCheckpoint ()
	{
		SqlExec (db, "BEGIN");
		try
		{
			std::vector<uint8_t> bytes;
			int64_t				 length = 0;
			uint32_t			 checksum = 0;
			bool				 present = false;
			{
				// length(blob) reads SQLite's size metadata without materializing an
				// oversized malicious blob. Both queries share one read transaction.
				Statement header (db, "SELECT format,bytes,checksum,length(data),typeof(data) FROM world_checkpoint WHERE id=1");
				int		  status = sqlite3_step (header.statement);
				SqlCheck (status, db, "read checkpoint header");
				if (status == SQLITE_ROW)
				{
					for (int column = 0; column < 4; ++column)
						if (sqlite3_column_type (header.statement, column) != SQLITE_INTEGER)
							throw std::runtime_error ("invalid checkpoint metadata type");
					const int64_t version = sqlite3_column_int64 (header.statement, 0);
					length = sqlite3_column_int64 (header.statement, 1);
					const int64_t encodedChecksum = sqlite3_column_int64 (header.statement, 2);
					const int64_t actualLength = sqlite3_column_int64 (header.statement, 3);
					const char	 *type = reinterpret_cast<const char *> (sqlite3_column_text (header.statement, 4));
					if (version != 1)
						throw std::runtime_error ("unsupported world checkpoint version");
					if (length < 0 || static_cast<uint64_t> (length) > MaxCheckpointBytes || actualLength != length || !type || std::string (type) != "blob" ||
						encodedChecksum < 0 || static_cast<uint64_t> (encodedChecksum) > UINT32_MAX)
						throw std::runtime_error ("invalid world checkpoint size or checksum metadata");
					checksum = static_cast<uint32_t> (encodedChecksum);
					present = true;
				}
			}
			if (present)
			{
				Statement payload (db, "SELECT data FROM world_checkpoint WHERE id=1");
				int		  status = sqlite3_step (payload.statement);
				SqlCheck (status, db, "read world checkpoint");
				if (status != SQLITE_ROW || sqlite3_column_type (payload.statement, 0) != SQLITE_BLOB || sqlite3_column_bytes (payload.statement, 0) != length)
					throw std::runtime_error ("world checkpoint changed during read");
				const uint8_t *data = static_cast<const uint8_t *> (sqlite3_column_blob (payload.statement, 0));
				if (length && !data)
					throw std::runtime_error ("cannot read world checkpoint data");
				if (length)
					bytes.assign (data, data + length);
				if (CheckpointChecksum (bytes) != checksum)
					throw std::runtime_error ("world checkpoint checksum mismatch");
			}
			SqlExec (db, "COMMIT");
			return bytes;
		}
		catch (...)
		{
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
			throw;
		}
	}

	TerrainNeighborhood CollisionNeighbors (ChunkKey key) const
	{
		TerrainNeighborhood neighbors{};
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
				{
					ChunkKey adjacent;
					if (!CheckedAdd (key.x, x, adjacent.x) || !CheckedAdd (key.y, y, adjacent.y) || !CheckedAdd (key.z, z, adjacent.z))
						continue;
					auto found = resident.find (CanonicalWorldKey (config, adjacent));
					if (found != resident.end ())
						neighbors[(z + 1) * 9 + (y + 1) * 3 + x + 1] = found->second.chunk;
				}
		return neighbors;
	}

	static CollisionPatch
	BuildCollisionPatch (const WorldConfig &config, const TerrainNeighborhood &neighbors, ChunkKey key, uint64_t serial, const std::array<int, 3> &patch)
	{
		auto mesh = std::make_shared<ChunkMesh> ();
		mesh->key = key;
		mesh->serial = serial;
		const std::array<int, 3> first{patch[0] * CollisionCells, patch[1] * CollisionCells, patch[2] * CollisionCells};
		if (config.generator == Generator::Planet || (neighbors[13]->solids && !neighbors[13]->solids->empty ()))
			BuildPlanetSurfaceRegion (config, neighbors, first, {CollisionCells, CollisionCells, CollisionCells}, *mesh);
		else if (config.generator == Generator::Terrain)
			BuildSmoothTerrainRegion (config, neighbors, first, {CollisionCells, CollisionCells, CollisionCells}, *mesh);
		else
		{
			auto sample = [&] (std::array<int, 3> p)
			{
				int		  index = 13;
				const int strides[]{1, 3, 9};
				for (int a = 0; a < 3; ++a)
				{
					if (p[a] < 0)
					{
						index -= strides[a];
						p[a] += VoxelsPerChunk;
					}
					else if (p[a] >= VoxelsPerChunk)
					{
						index += strides[a];
						p[a] -= VoxelsPerChunk;
					}
				}
				return neighbors[index] ? neighbors[index]->cells.Get (p[0], p[1], p[2]) : Cell{};
			};
			for (int z = first[2]; z < first[2] + CollisionCells; ++z)
				for (int y = first[1]; y < first[1] + CollisionCells; ++y)
					for (int x = first[0]; x < first[0] + CollisionCells; ++x)
					{
						const std::array<int, 3> p{x, y, z};
						if (!sample (p).IsSolid ())
							continue;
						for (int axis = 0; axis < 3; ++axis)
							for (int sign : {-1, 1})
							{
								auto adjacent = p;
								adjacent[axis] += sign;
								if (sample (adjacent).IsSolid ())
									continue;
								const int	   u = (axis + 1) % 3, v = (axis + 2) % 3;
								const uint32_t base = uint32_t (mesh->vertices.size ());
								for (int i = 0; i < 4; ++i)
								{
									MeshVertex vertex;
									for (int a = 0; a < 3; ++a)
										vertex.position[a] = float (p[a] * VoxelSize);
									vertex.position[axis] += sign > 0 ? VoxelSize : 0;
									vertex.position[u] += i == 1 || i == 2 ? VoxelSize : 0;
									vertex.position[v] += i >= 2 ? VoxelSize : 0;
									vertex.normal = {};
									vertex.normal[axis] = float (sign);
									mesh->vertices.push_back (vertex);
								}
								for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
									mesh->indices.push_back (base + (sign > 0 && i ? 4 - i : i));
							}
					}
		}
		CollisionPatch			 result{mesh, 0, {}};
		const std::array<int, 3> firstSample{first[0] - 1, first[1] - 1, first[2] - 1};
		if (config.generator == Generator::Planet || (neighbors[13]->solids && !neighbors[13]->solids->empty ()))
			SamplePlanetSurfaceLattice (config, neighbors, firstSample, CollisionSamples, result.density);
		else if (config.generator == Generator::Terrain)
		{
			result.density.resize (CollisionSamples * CollisionSamples * CollisionSamples);
			for (int y = 0; y < CollisionSamples; ++y)
				for (int x = 0; x < CollisionSamples; ++x)
				{
					const auto column = TerrainSampleColumn (config, key, firstSample[0] + x, firstSample[1] + y);
					for (int z = 0; z < CollisionSamples; ++z)
						result.density[(z * CollisionSamples + y) * CollisionSamples + x] =
							TerrainSampleDensity (config, key, {firstSample[0] + x, firstSample[1] + y, firstSample[2] + z}, neighbors, column).density;
				}
		}
		return result;
	}

	void CacheCollision (const CollisionId &id, CollisionPatch patch)
	{
		if (auto previous = collisionPatches.find (id); previous != collisionPatches.end ())
		{
			collisionBytes -= previous->second.Bytes ();
			collisionPatches.erase (previous);
		}
		const size_t bytes = patch.Bytes ();
		if (bytes > CollisionBudget)
			return;
		while (!collisionPatches.empty () && (collisionPatches.size () >= 512 || collisionBytes + bytes > CollisionBudget))
		{
			auto oldest = std::min_element (
				collisionPatches.begin (), collisionPatches.end (), [] (const auto &a, const auto &b) { return a.second.touched < b.second.touched; });
			collisionBytes -= oldest->second.Bytes ();
			collisionPatches.erase (oldest);
		}
		patch.touched = ++clock;
		collisionBytes += bytes;
		collisionPatches.emplace (id, std::move (patch));
	}

	void PumpCollision (size_t maxCompletions = CollisionJobLimit)
	{
		for (auto it = collisionPending.begin (); it != collisionPending.end () && maxCompletions;)
		{
			const CollisionId id = it->first;
			auto			  entry = resident.find ({id[0], id[1], id[2]});
			const bool		  current = entry != resident.end () && entry->second.meshSerial == it->second.serial;
			if (!current)
			{
				it->second.canceled->store (true);
				collisionCondition.notify_all ();
			}
			if (it->second.future.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
			{
				++it;
				continue;
			}
			try
			{
				auto result = it->second.future.get ();
				if (result)
				{
					{
						std::lock_guard lock (jobMutex);
						collisionResultBytes -= result->Bytes ();
					}
					collisionCondition.notify_all ();
					++collisionBuilds;
					++collisionWorkerBuilds;
					if (current)
						CacheCollision (id, std::move (*result));
					else
						++collisionStaleResults;
				}
				else
					++collisionStaleResults;
			}
			catch (const std::exception &e)
			{
				error = e.what ();
				if (current)
				{
					if (collisionFailures.size () >= 512)
						collisionFailures.clear ();
					collisionFailures[id] = it->second.serial;
				}
			}
			collisionPendingBytes -= it->second.inputBytes;
			it = collisionPending.erase (it);
			--maxCompletions;
		}
	}

	std::shared_ptr<const ChunkMesh> CollisionMesh (ChunkKey key, const std::array<int, 3> &patch, bool asynchronous = false)
	{
		key = CanonicalWorldKey (config, key);
		auto entry = resident.find (key);
		if (entry == resident.end ())
			return nullptr;
		const CollisionId id{key.x, key.y, key.z, patch[0], patch[1], patch[2]};
		const uint64_t	  revision = entry->second.meshSerial;
		if (auto cached = collisionPatches.find (id); cached != collisionPatches.end () && cached->second.mesh->serial == revision)
		{
			cached->second.touched = ++clock;
			return cached->second.mesh;
		}
		if (asynchronous)
		{
			if (auto pending = collisionPending.find (id); pending != collisionPending.end ())
			{
				if (pending->second.serial != revision)
				{
					pending->second.canceled->store (true);
					collisionCondition.notify_all ();
				}
				return nullptr;
			}
			const auto failed = collisionFailures.find (id);
			if (collisionPending.size () >= std::min (config.pendingLimit, CollisionJobLimit) ||
				(failed != collisionFailures.end () && failed->second == revision))
				return nullptr;
			auto   neighbors = CollisionNeighbors (key);
			size_t inputBytes = 0;
			for (const auto &chunk : neighbors)
				if (chunk)
					inputBytes += ChunkResidentBytes (*chunk);
			if (inputBytes > CollisionInputBudget - collisionPendingBytes)
				return nullptr;
			auto			  canceled = std::make_shared<std::atomic<bool>> (false);
			const WorldConfig immutableConfig = config;
			auto			  task = std::make_shared<std::packaged_task<std::shared_ptr<CollisionPatch> ()>> (
				 [this, immutableConfig, neighbors, key, patch, revision, canceled] () -> std::shared_ptr<CollisionPatch>
				 {
					 if (canceled->load ())
						 return {};
					 auto		  result = std::make_shared<CollisionPatch> (BuildCollisionPatch (immutableConfig, neighbors, key, revision, patch));
					 const size_t bytes = result->Bytes ();
					 if (bytes > CollisionBudget)
						 throw std::runtime_error ("Collision patch exceeds asynchronous result budget");
					 std::unique_lock lock (jobMutex);
					 collisionCondition.wait (lock, [&] { return stopping || canceled->load () || collisionResultBytes + bytes <= CollisionBudget; });
					 if (stopping || canceled->load ())
						 return {};
					 collisionResultBytes += bytes;
					 return result;
				 });
			auto future = task->get_future ();
			collisionPending.emplace (id, CollisionJob{revision, inputBytes, canceled, std::move (future)});
			collisionPendingBytes += inputBytes;
			{
				std::lock_guard lock (jobMutex);
				collisionJobs.emplace_back ([task] { (*task) (); });
			}
			collisionCondition.notify_all ();
			return nullptr;
		}
		auto result = BuildCollisionPatch (config, CollisionNeighbors (key), key, revision, patch);
		auto mesh = result.mesh;
		++collisionBuilds;
		++collisionSyncBuilds;
		CacheCollision (id, std::move (result));
		return mesh;
	}

	void Invalidate (ChunkKey key)
	{
		// Smooth density/normal probes cross edges and corners as well as faces.
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
				{
					ChunkKey neighbor;
					if (!CheckedAdd (key.x, x, neighbor.x) || !CheckedAdd (key.y, y, neighbor.y) || !CheckedAdd (key.z, z, neighbor.z))
						continue;
					auto adjacent = resident.find (CanonicalWorldKey (config, neighbor));
					if (adjacent != resident.end ())
					{
						if (adjacent->second.failedMeshSerial == adjacent->second.meshSerial)
							adjacent->second.meshFootprint.reset ();
						adjacent->second.meshSerial = ++serial;
					}
				}
		for (auto &[id, job] : collisionPending)
		{
			auto entry = resident.find ({id[0], id[1], id[2]});
			if (entry == resident.end () || entry->second.meshSerial != job.serial)
				job.canceled->store (true);
		}
		collisionCondition.notify_all ();
	}

	bool Protected (ChunkKey key) const
	{
		return pinned.contains (key) || edits.contains (key);
	}

	size_t ReservedBytes (ChunkKey key, const Entry &entry) const
	{
		auto edit = edits.find (key);
		return edit == edits.end () ? entry.treeBytes : std::max (entry.treeBytes, edit->second.bytes);
	}

	bool CanAdmit (const Chunk &chunk, size_t bytes)
	{
		if (bytes > config.residentBytes)
		{
			error = "chunk exceeds resident memory budget";
			return false;
		}
		size_t minimum = bytes;
		for (const auto &[key, entry] : resident)
			if (key != chunk.key && Protected (key))
				minimum += ReservedBytes (key, entry);
		if (minimum > config.residentBytes)
		{
			error = "pinned chunks exceed resident memory budget";
			return false;
		}
		return true;
	}

	void Trim ()
	{
		size_t bytes = 0;
		for (const auto &[key, entry] : resident)
			bytes += entry.treeBytes;
		while (resident.size () > config.residentLimit || bytes > config.residentBytes)
		{
			auto oldest = resident.end ();
			for (auto it = resident.begin (); it != resident.end (); ++it)
				if (!Protected (it->first) && (oldest == resident.end () || it->second.touched < oldest->second.touched))
					oldest = it;
			if (oldest == resident.end ())
				break;
			ChunkKey key = oldest->first;
			bytes -= oldest->second.treeBytes;
			resident.erase (oldest);
			Invalidate (key);
		}
	}

	void CacheMesh (Entry &target, std::shared_ptr<ChunkMesh> mesh)
	{
		size_t bytes = mesh->Bytes ();
		target.meshFootprint = bytes;
		if (bytes > config.meshBytes)
		{
			error = "chunk mesh exceeds CPU mesh budget";
			target.failedMeshSerial = mesh->serial;
			return;
		}
		target.mesh.reset ();
		for (const auto &[key, entry] : resident)
			if (entry.mesh)
				bytes += entry.mesh->Bytes ();
		while (bytes > config.meshBytes)
		{
			auto oldest = resident.end ();
			for (auto it = resident.begin (); it != resident.end (); ++it)
				if (it->second.mesh && (oldest == resident.end () || it->second.touched < oldest->second.touched))
					oldest = it;
			if (oldest == resident.end ())
				break;
			bytes -= oldest->second.mesh->Bytes ();
			oldest->second.mesh.reset ();
		}
		target.mesh = std::move (mesh);
	}

	static std::shared_ptr<ChunkMesh> BuildMesh (const std::array<std::shared_ptr<const Chunk>, 27> &chunks, uint64_t meshSerial, const WorldConfig &config)
	{
		auto result = std::make_shared<ChunkMesh> ();
		result->key = chunks[13]->key;
		result->serial = meshSerial;
		if (config.generator == Generator::Planet || (chunks[13]->solids && !chunks[13]->solids->empty ()))
		{
			BuildPlanetSurface (config, chunks, *result);
			return result;
		}
		struct Face
		{
			uint16_t plane;
			uint8_t	 u, v;
			Cell	 cell;
		};
		std::vector<Face> faces;
		auto			  sample = [&] (std::array<int, 3> p) -> Cell
		{
			int			  index = 13;
			constexpr int strides[] = {1, 3, 9};
			for (int axis = 0; axis < 3; ++axis)
			{
				if (p[axis] < 0)
				{
					index -= strides[axis];
					p[axis] += VoxelsPerChunk;
				}
				else if (p[axis] >= VoxelsPerChunk)
				{
					index += strides[axis];
					p[axis] -= VoxelsPerChunk;
				}
			}
			return chunks[index] ? chunks[index]->cells.Get (p[0], p[1], p[2]) : Cell{};
		};
		chunks[13]->cells.Visit (
			[&] (int x, int y, int z, int size, Cell cell)
			{
				if (cell.contents == Empty)
					return;
				std::array<int, 3> base = {x, y, z};
				for (int axis = 0; axis < 3; ++axis)
					for (int sign : {-1, 1})
					{
						int u = (axis + 1) % 3, v = (axis + 2) % 3;
						for (int a = 0; a < size; ++a)
							for (int b = 0; b < size; ++b)
							{
								auto p = base;
								p[u] += a;
								p[v] += b;
								p[axis] += sign > 0 ? size : -1;
								Cell adjacent = sample (p);
								if (adjacent.IsSolid () || (cell.contents != Solid && adjacent.contents == cell.contents))
									continue;
								if (faces.size () >= MaxMeshQuads)
									throw std::runtime_error ("chunk mesh exceeds 1000000 exposed faces");
								int plane = (axis * 2 + (sign > 0)) * (VoxelsPerChunk + 1) + p[axis] + (sign < 0);
								faces.push_back ({static_cast<uint16_t> (plane), static_cast<uint8_t> (p[u]), static_cast<uint8_t> (p[v]), cell});
							}
					}
			});
		// Coalesce each material plane into maximal homogeneous dyadic squares.
		// The renderer triangulates every square with finest-cell boundary points;
		// adjacent squares/chunks therefore share edges even with different sizes.
		std::sort (faces.begin (), faces.end (), [] (const Face &a, const Face &b) { return a.plane < b.plane; });
		std::array<Cell, VoxelsPerChunk * VoxelsPerChunk> mask;
		for (size_t begin = 0; begin < faces.size ();)
		{
			mask.fill ({});
			size_t end = begin;
			while (end < faces.size () && faces[end].plane == faces[begin].plane)
			{
				const Face &face = faces[end++];
				mask[face.v * VoxelsPerChunk + face.u] = face.cell;
			}
			const int							plane = faces[begin].plane % (VoxelsPerChunk + 1);
			const int							side = faces[begin].plane / (VoxelsPerChunk + 1);
			const int							axis = side / 2, u = (axis + 1) % 3, v = (axis + 2) % 3;
			std::function<void (int, int, int)> merge = [&] (int x, int y, int size)
			{
				const Cell cell = mask[y * VoxelsPerChunk + x];
				bool	   same = true;
				for (int a = y; a < y + size && same; ++a)
					for (int b = x; b < x + size; ++b)
						if (mask[a * VoxelsPerChunk + b] != cell)
						{
							same = false;
							break;
						}
				if (!same)
				{
					const int half = size / 2;
					merge (x, y, half);
					merge (x + half, y, half);
					merge (x, y + half, half);
					merge (x + half, y + half, half);
				}
				else if (cell.contents != Empty)
				{
					MeshQuad quad;
					quad.origin[axis] = float (plane * VoxelSize);
					quad.origin[u] = float (x * VoxelSize);
					quad.origin[v] = float (y * VoxelSize);
					quad.size = float (size * VoxelSize);
					quad.axis = uint8_t (axis);
					quad.sign = (side & 1) ? 1 : -1;
					quad.material = cell.material;
					quad.contents = cell.contents;
					result->quads.push_back (quad);
				}
			};
			merge (0, 0, VoxelsPerChunk);
			begin = end;
		}
		if (config.smoothTerrain && config.generator != Generator::Empty)
		{
			BuildSmoothTerrainMesh (config, chunks, *result);
			std::vector<MeshQuad> ().swap (result->quads);
		}
		return result;
	}
};

World::World (WorldConfig config) : impl (std::make_unique<Impl> (std::move (config))) {}
World::~World () = default;
const WorldConfig &World::Config () const
{
	return impl->config;
}
void World::SetSmoothTerrain (bool enabled)
{
	if (impl->config.smoothTerrain == enabled)
		return;
	impl->config.smoothTerrain = enabled;
	for (auto &[key, entry] : impl->resident)
	{
		entry.meshSerial = ++impl->serial;
		entry.meshFootprint.reset ();
	}
}
const std::string &World::LastError () const
{
	return impl->error;
}

bool World::Request (ChunkKey key)
{
	key = CanonicalWorldKey (impl->config, key);
	auto found = impl->resident.find (key);
	if (found != impl->resident.end ())
	{
		found->second.touched = ++impl->clock;
		return true;
	}
	if (impl->pending.contains (key))
		return true;
	// Ready futures retain decoded trees until Pump, independently of residency.
	if (impl->pending.size () >= std::min (impl->config.pendingLimit, size_t (4)))
		return false;
	impl->pending.emplace (key, impl->Submit ([state = impl.get (), key] { return state->Load (key); }));
	return true;
}

void World::Pump (size_t maxCompletions)
{
	auto notifications = std::move (impl->surfaceCompletions);
	impl->surfaceCompletions.clear ();
	for (auto &notify : notifications)
		notify ();
	struct Completion
	{
		EditCompletion callback;
		bool		   success;
		uint64_t	   revision;
		std::string	   error;
	};
	std::vector<Completion>				completions;
	std::vector<std::function<void ()>> pageCompletions;
	for (auto it = impl->edits.begin (); it != impl->edits.end () && maxCompletions;)
	{
		if (it->second.future.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
		{
			++it;
			continue;
		}
		bool		success = true;
		uint64_t	revision = it->second.previousRevision;
		std::string failure;
		try
		{
			it->second.future.get ();
			// Queued edits protect their resident entry against eviction and
			// conflicting writes until this publication point.
			auto &entry = impl->resident.at (it->first);
			entry.chunk = it->second.chunk;
			entry.treeBytes = it->second.bytes;
			revision = entry.chunk->revision;
			impl->Invalidate (it->first);
		}
		catch (const std::exception &error)
		{
			success = false;
			failure = impl->error = error.what ();
		}
		completions.push_back ({std::move (it->second.completion), success, revision, std::move (failure)});
		impl->pendingEditBytes -= it->second.bytes;
		it = impl->edits.erase (it);
		--maxCompletions;
	}
	for (auto it = impl->pageJobs.begin (); it != impl->pageJobs.end () && maxCompletions;)
	{
		if (it->future.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
		{
			++it;
			continue;
		}
		std::string failure;
		try
		{
			it->future.get ();
		}
		catch (const std::exception &error)
		{
			failure = impl->error = error.what ();
		}
		pageCompletions.push_back (
			[callback = std::move (it->completion), failure = std::move (failure)]
			{
				if (callback)
					callback (failure);
			});
		impl->pendingPageBytes -= it->bytes;
		if (it->writeId)
			impl->pageWrites.erase (it->writeId);
		it = impl->pageJobs.erase (it);
		--maxCompletions;
	}
	for (auto it = impl->pending.begin (); it != impl->pending.end () && maxCompletions;)
	{
		if (it->second.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
		{
			++it;
			continue;
		}
		try
		{
			auto   chunk = it->second.get ();
			size_t bytes = ChunkResidentBytes (*chunk);
			if (impl->CanAdmit (*chunk, bytes))
			{
				impl->resident[it->first] = {std::move (chunk), nullptr, ++impl->clock, ++impl->serial, bytes};
				impl->Invalidate (it->first);
			}
		}
		catch (const std::exception &e)
		{
			impl->error = e.what ();
		}
		it = impl->pending.erase (it);
		--maxCompletions;
	}
	for (auto it = impl->meshes.begin (); it != impl->meshes.end () && maxCompletions;)
	{
		if (it->second.future.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
		{
			++it;
			continue;
		}
		try
		{
			auto mesh = it->second.future.get ();
			auto found = impl->resident.find (it->first);
			if (found != impl->resident.end () && found->second.meshSerial == mesh->serial)
				impl->CacheMesh (found->second, std::move (mesh));
		}
		catch (const std::exception &e)
		{
			impl->error = e.what ();
			auto found = impl->resident.find (it->first);
			if (found != impl->resident.end () && found->second.meshSerial == it->second.serial)
			{
				found->second.failedMeshSerial = it->second.serial;
				found->second.meshFootprint = std::numeric_limits<size_t>::max ();
			}
		}
		it = impl->meshes.erase (it);
		--maxCompletions;
	}
	impl->PumpCollision ();
	impl->Trim ();
	// Callbacks may enqueue further edits. Never invoke them while iterating the
	// job maps, and never access world state after invoking user callbacks.
	for (auto &completion : completions)
		if (completion.callback)
			completion.callback (completion.success, completion.revision, completion.error);
	for (auto &completion : pageCompletions)
		completion ();
}

bool World::Require (ChunkKey key, std::string *error)
{
	key = CanonicalWorldKey (impl->config, key);
	// Collision-critical callers may wait for a queue slot; ordinary Request
	// callers remain nonblocking and retry from their interest list next frame.
	if (!impl->resident.contains (key) && !impl->pending.contains (key) && impl->pending.size () >= std::min (impl->config.pendingLimit, size_t (4)))
	{
		impl->pending.begin ()->second.wait ();
		Pump (impl->config.pendingLimit * 2 + 1);
	}
	if (!impl->resident.contains (key) && !impl->pending.contains (key))
		impl->pending.emplace (key, impl->Submit ([state = impl.get (), key] { return state->Load (key); }, true));
	if (!Request (key))
	{
		impl->error = "chunk request queue is full";
		if (error)
			*error = impl->error;
		return false;
	}
	auto pending = impl->pending.find (key);
	if (pending != impl->pending.end ())
		pending->second.wait ();
	Pump (impl->config.pendingLimit * 2 + 1);
	if (impl->resident.contains (key))
		return true;
	if (impl->error.empty ())
		impl->error = "chunk cannot remain resident within the configured budget";
	if (error)
		*error = impl->error;
	return false;
}

bool World::SetInterest (const std::vector<ChunkKey> &keys)
{
	std::unordered_set<ChunkKey, KeyHash> candidate;
	for (auto key : keys)
		candidate.insert (CanonicalWorldKey (impl->config, key));
	if (candidate.size () > impl->config.residentLimit)
		return false;
	size_t reserved = 0;
	for (const auto &[key, entry] : impl->resident)
		if (candidate.contains (key) || impl->edits.contains (key))
			reserved += impl->ReservedBytes (key, entry);
	if (reserved > impl->config.residentBytes)
		return false;
	impl->pinned = std::move (candidate);
	impl->Trim ();
	return true;
}

std::shared_ptr<const Chunk> World::Find (ChunkKey key) const
{
	key = CanonicalWorldKey (impl->config, key);
	auto found = impl->resident.find (key);
	if (found == impl->resident.end ())
		return nullptr;
	found->second.touched = ++impl->clock;
	return found->second.chunk;
}

std::vector<ChunkKey> World::ResidentKeys () const
{
	std::vector<ChunkKey> keys;
	keys.reserve (impl->resident.size ());
	for (const auto &[key, entry] : impl->resident)
		keys.push_back (key);
	return keys;
}

Cell World::Sample (const WorldPosition &position) const
{
	WorldPosition canonical = position;
	if (!Normalize (canonical))
		return {0, Unknown};
	auto chunk = Find (canonical.chunk);
	if (!chunk)
		return {0, Unknown};
	return chunk->cells.Get (
		static_cast<int> (canonical.local[0]) / VoxelSize, static_cast<int> (canonical.local[1]) / VoxelSize,
		static_cast<int> (canonical.local[2]) / VoxelSize);
}

bool World::Edit (const WorldPosition &position, Cell cell, std::string *error)
{
	WorldPosition canonical = position;
	if (!ValidCell (cell) || !Normalize (canonical))
	{
		impl->error = "invalid edit position or contents";
		if (error)
			*error = impl->error;
		return false;
	}
	canonical.chunk = CanonicalWorldKey (impl->config, canonical.chunk);
	if (impl->edits.contains (canonical.chunk))
	{
		impl->error = "chunk has a pending durable edit";
		if (error)
			*error = impl->error;
		return false;
	}
	if (!Require (canonical.chunk, error))
		return false;
	// Require may Pump unrelated completions whose callbacks enqueue an edit.
	canonical.chunk = CanonicalWorldKey (impl->config, canonical.chunk);
	if (impl->edits.contains (canonical.chunk))
	{
		impl->error = "chunk has a pending durable edit";
		if (error)
			*error = impl->error;
		return false;
	}
	auto chunk = std::make_shared<Chunk> (*impl->resident.at (canonical.chunk).chunk);
	if (!chunk->cells.Set (
			static_cast<int> (canonical.local[0]) / VoxelSize, static_cast<int> (canonical.local[1]) / VoxelSize,
			static_cast<int> (canonical.local[2]) / VoxelSize, cell))
		return true;
	if (chunk->revision >= static_cast<uint64_t> (INT64_MAX))
	{
		impl->error = "chunk revision exhausted";
		if (error)
			*error = impl->error;
		return false;
	}
	++chunk->revision;
	size_t bytes = ChunkResidentBytes (*chunk);
	if (!impl->CanAdmit (*chunk, bytes))
	{
		if (error)
			*error = impl->error;
		return false;
	}
	try
	{
		impl->Submit ([state = impl.get (), chunk] { state->Store (*chunk); }, true).get ();
	}
	catch (const std::exception &e)
	{
		impl->error = e.what ();
		if (error)
			*error = impl->error;
		return false;
	}
	impl->resident.at (canonical.chunk).chunk = std::move (chunk);
	impl->resident.at (canonical.chunk).treeBytes = bytes;
	impl->Invalidate (canonical.chunk);
	impl->Trim ();
	return true;
}

bool World::EditAsync (const WorldPosition &position, Cell cell, EditCompletion completion, std::string *error)
{
	auto fail = [&] (const char *message)
	{
		impl->error = message;
		if (error)
			*error = impl->error;
		return false;
	};
	WorldPosition canonical = position;
	if (!ValidCell (cell) || !Normalize (canonical))
		return fail ("invalid edit position or contents");
	canonical.chunk = CanonicalWorldKey (impl->config, canonical.chunk);
	if (impl->edits.contains (canonical.chunk))
		return fail ("chunk has a pending durable edit");
	if (impl->edits.size () >= std::min (impl->config.pendingLimit, size_t (64)))
		return fail ("durable edit queue is full");
	auto found = impl->resident.find (canonical.chunk);
	if (found == impl->resident.end ())
		return fail ("asynchronous editing requires resident terrain");
	auto		   chunk = std::make_shared<Chunk> (*found->second.chunk);
	const uint64_t previousRevision = chunk->revision;
	const bool	   changed = chunk->cells.Set (
		static_cast<int> (canonical.local[0]) / VoxelSize, static_cast<int> (canonical.local[1]) / VoxelSize, static_cast<int> (canonical.local[2]) / VoxelSize,
		cell);
	if (changed && chunk->revision >= static_cast<uint64_t> (INT64_MAX))
		return fail ("chunk revision exhausted");
	if (changed)
		++chunk->revision;
	const size_t bytes = ChunkResidentBytes (*chunk);
	if (bytes > impl->config.residentBytes - impl->pendingEditBytes)
		return fail ("durable edit snapshots exceed memory budget");
	if (!impl->CanAdmit (*chunk, bytes))
	{
		if (error)
			*error = impl->error;
		return false;
	}
	auto future = impl->Submit (
		[state = impl.get (), chunk, changed]
		{
			if (changed)
				state->Store (*chunk);
		},
		true);
	impl->edits.emplace (canonical.chunk, Impl::EditJob{std::move (chunk), std::move (future), std::move (completion), bytes, previousRevision});
	impl->pendingEditBytes += bytes;
	found->second.touched = ++impl->clock;
	return true;
}

std::shared_ptr<const ChunkMesh> World::Mesh (ChunkKey key, bool wait)
{
	key = CanonicalWorldKey (impl->config, key);
	for (;;)
	{
		auto found = impl->resident.find (key);
		if (found == impl->resident.end ())
			return nullptr;
		found->second.touched = ++impl->clock;
		if (found->second.failedMeshSerial == found->second.meshSerial)
			return nullptr;
		if (found->second.mesh && found->second.mesh->serial == found->second.meshSerial)
			return found->second.mesh;
		if (!impl->meshes.contains (key) && impl->meshes.size () < std::min (impl->config.pendingLimit, size_t (4)))
		{
			std::array<std::shared_ptr<const Chunk>, 27> chunks{};
			for (int z = -1; z <= 1; ++z)
				for (int y = -1; y <= 1; ++y)
					for (int x = -1; x <= 1; ++x)
					{
						ChunkKey adjacent;
						if (CheckedAdd (key.x, x, adjacent.x) && CheckedAdd (key.y, y, adjacent.y) && CheckedAdd (key.z, z, adjacent.z))
							chunks[(z + 1) * 9 + (y + 1) * 3 + x + 1] = Find (adjacent);
					}
			uint64_t		  serial = found->second.meshSerial;
			const WorldConfig config = impl->config;
			impl->meshes.emplace (key, Impl::MeshJob{serial, impl->SubmitMesh ([chunks, serial, config] { return Impl::BuildMesh (chunks, serial, config); })});
		}
		if (wait)
		{
			auto pending = impl->meshes.find (key);
			if (pending != impl->meshes.end ())
				pending->second.future.wait ();
			else if (!impl->meshes.empty ())
				impl->meshes.begin ()->second.future.wait ();
			Pump (impl->config.pendingLimit * 2 + 1);
			continue;
		}
		return found == impl->resident.end () ? nullptr : found->second.mesh;
	}
}

std::optional<size_t> World::MeshFootprint (ChunkKey key) const
{
	key = CanonicalWorldKey (impl->config, key);
	auto found = impl->resident.find (key);
	return found == impl->resident.end () ? std::nullopt : found->second.meshFootprint;
}

bool World::Flush (std::string *error)
{
	try
	{
		impl->Submit ([this]
					  { SqlCheck (sqlite3_wal_checkpoint_v2 (impl->db, nullptr, SQLITE_CHECKPOINT_FULL, nullptr, nullptr), impl->db, "checkpoint world"); })
			.get ();
		return true;
	}
	catch (const std::exception &e)
	{
		impl->error = e.what ();
		if (error)
			*error = impl->error;
		return false;
	}
}

bool World::EditSurfaceAsync (const WorldPosition &position, Cell cell, EditCompletion completion, std::string *error)
{
	auto chunk = Find (position.chunk);
	if (impl->config.generator != Generator::Planet && (!chunk || !chunk->solids || chunk->solids->empty ()))
		return EditAsync (position, cell, std::move (completion), error);
	if (!chunk || (cell.contents != Empty && cell.contents != Solid))
	{
		if (error)
			*error = "unsupported surface edit";
		return false;
	}
	WorldPosition center = position;
	if (!Normalize (center))
		return false;
	for (auto &a : center.local)
		a = float (std::floor (a / VoxelSize) * VoxelSize + VoxelSize / 2);
	SolidDefinition solid;
	if (!MakeSolidBox (center, {VoxelSize, VoxelSize, VoxelSize}, {}, cell.material ? cell.material : 3, solid, error))
		return false;
	solid.operation = cell.contents == Empty ? SolidOperation::Subtract : SolidOperation::Add;
	if (!AddSolid (solid, error))
		return false;
	const uint64_t revision = chunk->revision + 1;
	impl->surfaceCompletions.push_back (
		[completion = std::move (completion), revision]
		{
			if (completion)
				completion (true, revision, {});
		});
	return true;
}

bool World::AddSolid (const SolidDefinition &input, std::string *output)
{
	try
	{
		SolidDefinition solid = input;
		if (!ValidateSolid (solid, &impl->error))
			throw std::runtime_error (impl->error);
		solid.origin.chunk = CanonicalWorldKey (impl->config, solid.origin.chunk);
		SolidPoint low = solid.vertices.front (), high = low;
		for (const auto &p : solid.vertices)
			for (int a = 0; a < 3; ++a)
			{
				low[a] = std::min (low[a], p[a]);
				high[a] = std::max (high[a], p[a]);
			}
		WorldPosition first, last;
		for (int a = 0; a < 3; ++a)
		{
			low[a] -= VoxelSize * 2;
			high[a] += VoxelSize * 2;
		}
		if (!Add (solid.origin, low, first) || !Add (solid.origin, high, last))
			throw std::runtime_error ("solid exceeds world coordinates");
		size_t count = 1;
		for (int a = 0; a < 3; ++a)
		{
			auto n = last.chunk[a] - first.chunk[a] + 1;
			if (n < 1 || n > 256)
				throw std::runtime_error ("solid spans too many chunks");
			count *= size_t (n);
		}
		if (count > 256)
			throw std::runtime_error ("solid edit exceeds 256-chunk transaction limit");
		if (!Flush (output))
			return false;
		std::vector<ChunkKey> keys;
		for (int64_t z = 0; z <= last.chunk.z - first.chunk.z; ++z)
			for (int64_t y = 0; y <= last.chunk.y - first.chunk.y; ++y)
				for (int64_t x = 0; x <= last.chunk.x - first.chunk.x; ++x)
					keys.push_back (CanonicalWorldKey (impl->config, {first.chunk.x + x, first.chunk.y + y, first.chunk.z + z}));
		for (auto key : keys)
			if (impl->edits.contains (key))
				throw std::runtime_error ("solid overlaps a pending durable edit");
		// A committed import must remain admissible alongside protected chunks.
		// Unpinned neighbors can still be streamed independently within the cache.
		size_t protectedBytes = 0;
		for (const auto &[key, entry] : impl->resident)
			if (impl->Protected (key) && std::find (keys.begin (), keys.end (), key) == keys.end ())
				protectedBytes += impl->ReservedBytes (key, entry);
		impl->Submit (
				[state = impl.get (), solid, keys, protectedBytes] () mutable
				{
					SqlExec (state->db, "BEGIN IMMEDIATE");
					try
					{
						for (auto key : keys)
						{
							auto chunk = state->Load (key);
							auto solids = std::make_shared<std::vector<SolidDefinition>> (chunk->solids ? *chunk->solids : std::vector<SolidDefinition>{});
							if (solids->size () >= 256)
								throw std::runtime_error ("chunk solid operation budget exceeded");
							solids->push_back (solid);
							chunk->solids = solids;
							if (chunk->revision >= uint64_t (INT64_MAX))
								throw std::runtime_error ("chunk revision exhausted");
							++chunk->revision;
							const size_t bytes = ChunkResidentBytes (*chunk);
							if (bytes > state->config.residentBytes)
								throw std::runtime_error ("solid edit exceeds resident chunk memory budget");
							if (state->pinned.contains (key))
								protectedBytes += bytes;
							if (protectedBytes > state->config.residentBytes)
								throw std::runtime_error ("solid edit exceeds pinned chunk memory budget");
							state->StoreTerrain (*chunk);
						}
						SqlExec (state->db, "COMMIT");
						state->RefreshContentHead ();
					}
					catch (...)
					{
						sqlite3_exec (state->db, "ROLLBACK", nullptr, nullptr, nullptr);
						throw;
					}
				},
				true)
			.get ();
		for (auto key : keys)
		{
			impl->pending.erase (key);
			impl->meshes.erase (key);
			impl->resident.erase (key);
			impl->Invalidate (key);
			Request (key);
		}
		return true;
	}
	catch (const std::exception &e)
	{
		impl->error = e.what ();
		if (output)
			*output = impl->error;
		return false;
	}
}

bool World::SaveCheckpoint (const std::vector<uint8_t> &bytes, std::string *error)
{
	if (bytes.size () > MaxCheckpointBytes)
	{
		impl->error = "world checkpoint exceeds 64 MiB";
		if (error)
			*error = impl->error;
		return false;
	}
	if (!Flush (error))
		return false;
	Pump (impl->edits.size () + impl->pageJobs.size () + impl->pending.size () + impl->meshes.size ());
	if (!impl->edits.empty () || !impl->pageWrites.empty ())
	{
		impl->error = "cannot save checkpoint while completion callbacks queue durable writes";
		if (error)
			*error = impl->error;
		return false;
	}
	try
	{
		impl->Submit ([state = impl.get (), &bytes] { state->StoreCheckpoint (bytes); }, true).get ();
		return true;
	}
	catch (const std::exception &exception)
	{
		impl->error = exception.what ();
		if (error)
			*error = impl->error;
		return false;
	}
}

bool World::LoadCheckpoint (std::vector<uint8_t> &bytes, std::string *error)
{
	try
	{
		auto loaded = impl->Submit ([state = impl.get ()] { return state->ReadCheckpoint (); }, true).get ();
		bytes = std::move (loaded);
		return true;
	}
	catch (const std::exception &exception)
	{
		impl->error = exception.what ();
		if (error)
			*error = impl->error;
		return false;
	}
}

bool World::StoreEntityPage (uint64_t id, ChunkKey key, const std::vector<uint8_t> &bytes, uint64_t expectedRevision, uint64_t *newRevision, std::string *error)
{
	key = CanonicalWorldKey (impl->config, key);
	if (!id || bytes.size () > MaxEntityPageBytes || expectedRevision >= uint64_t (INT64_MAX))
		return impl->PageFailure ("invalid entity page ID, size or revision", error);
	if (impl->pageWrites.contains (id))
		return impl->PageFailure ("entity page has a pending write", error);
	try
	{
		auto revision =
			impl->Submit ([state = impl.get (), id, key, &bytes, expectedRevision] { return state->StorePage (id, key, bytes, expectedRevision, false); }, true)
				.get ();
		if (newRevision)
			*newRevision = revision;
		return true;
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::StoreEntityPageAsync (
	uint64_t id, ChunkKey key, const std::vector<uint8_t> &bytes, uint64_t expectedRevision, EditCompletion completion, std::string *error)
{
	key = CanonicalWorldKey (impl->config, key);
	if (!id || bytes.size () > MaxEntityPageBytes || expectedRevision >= uint64_t (INT64_MAX))
		return impl->PageFailure ("invalid entity page ID, size or revision", error);
	try
	{
		auto revision = std::make_shared<uint64_t> (expectedRevision);
		return impl->QueuePageJob (
			id, bytes.size (),
			[state = impl.get (), id, key, bytes, expectedRevision, revision] { *revision = state->StorePage (id, key, bytes, expectedRevision, false); },
			[revision, completion = std::move (completion)] (const std::string &failure)
			{
				if (completion)
					completion (failure.empty (), *revision, failure);
			},
			error);
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::DeleteEntityPage (uint64_t id, uint64_t expectedRevision, uint64_t *newRevision, std::string *error)
{
	if (!id || !expectedRevision || expectedRevision >= uint64_t (INT64_MAX))
		return impl->PageFailure ("invalid entity page ID or revision", error);
	if (impl->pageWrites.contains (id))
		return impl->PageFailure ("entity page has a pending write", error);
	try
	{
		auto revision =
			impl->Submit ([state = impl.get (), id, expectedRevision] { return state->StorePage (id, {}, {}, expectedRevision, true); }, true).get ();
		if (newRevision)
			*newRevision = revision;
		return true;
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::DeleteEntityPageAsync (uint64_t id, uint64_t expectedRevision, EditCompletion completion, std::string *error)
{
	if (!id || !expectedRevision || expectedRevision >= uint64_t (INT64_MAX))
		return impl->PageFailure ("invalid entity page ID or revision", error);
	try
	{
		auto revision = std::make_shared<uint64_t> (expectedRevision);
		return impl->QueuePageJob (
			id, 0, [state = impl.get (), id, expectedRevision, revision] { *revision = state->StorePage (id, {}, {}, expectedRevision, true); },
			[revision, completion = std::move (completion)] (const std::string &failure)
			{
				if (completion)
					completion (failure.empty (), *revision, failure);
			},
			error);
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

uint64_t World::AllocateEntityPageId (std::string *error)
{
	try
	{
		return impl->Submit ([state = impl.get ()] { return state->AllocatePageId (); }, true).get ();
	}
	catch (const std::exception &exception)
	{
		impl->PageFailure (exception.what (), error);
		return 0;
	}
}

bool World::LoadEntityPage (uint64_t id, EntityPage &page, std::string *error)
{
	if (!id)
		return impl->PageFailure ("invalid entity page ID", error);
	try
	{
		auto loaded = impl->Submit ([state = impl.get (), id] { return state->ReadPage (id); }, true).get ();
		if (!loaded)
			return impl->PageFailure ("entity page not found", error);
		page = std::move (*loaded);
		return true;
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::LoadEntityPageAsync (uint64_t id, EntityPageLoadCompletion completion, std::string *error)
{
	if (!id)
		return impl->PageFailure ("invalid entity page ID", error);
	try
	{
		auto result = std::make_shared<std::optional<EntityPage>> ();
		return impl->QueuePageJob (
			0, MaxEntityPageBytes, [state = impl.get (), id, result] { *result = state->ReadPage (id); },
			[result, completion = std::move (completion)] (const std::string &failure)
			{
				if (completion)
					completion (std::move (*result), failure);
			},
			error);
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

std::vector<EntityPageInfo> World::ListEntityPages (ChunkKey minimum, ChunkKey maximum, size_t limit, std::string *error, uint64_t afterId)
{
	if (error)
		error->clear ();
	if (!ValidPageBounds (minimum, maximum, limit))
	{
		impl->PageFailure ("invalid entity page bounds or limit", error);
		return {};
	}
	try
	{
		return impl->Submit ([state = impl.get (), minimum, maximum, limit, afterId] { return state->ListPages (minimum, maximum, limit, afterId); }, true)
			.get ();
	}
	catch (const std::exception &exception)
	{
		impl->PageFailure (exception.what (), error);
		return {};
	}
}

bool World::ListEntityPagesAsync (ChunkKey minimum, ChunkKey maximum, size_t limit, EntityPageListCompletion completion, std::string *error, uint64_t afterId)
{
	if (!ValidPageBounds (minimum, maximum, limit))
		return impl->PageFailure ("invalid entity page bounds or limit", error);
	try
	{
		auto result = std::make_shared<std::vector<EntityPageInfo>> ();
		return impl->QueuePageJob (
			0, limit * sizeof (EntityPageInfo),
			[state = impl.get (), minimum, maximum, limit, afterId, result] { *result = state->ListPages (minimum, maximum, limit, afterId); },
			[result, completion = std::move (completion)] (const std::string &failure)
			{
				if (completion)
					completion (std::move (*result), failure);
			},
			error);
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::RestoreEntityPagesCheckpoint (std::string *error)
{
	if (!impl->pageJobs.empty () || !impl->edits.empty ())
		return impl->PageFailure ("cannot restore checkpoint with pending durable operations", error);
	try
	{
		impl->Submit ([state = impl.get ()] { state->RestorePages (); }, true).get ();
		return true;
	}
	catch (const std::exception &exception)
	{
		return impl->PageFailure (exception.what (), error);
	}
}

bool World::SaveAs (const std::string &databasePath, std::string *error)
{
	if (databasePath.empty ())
	{
		impl->error = "world save requires a database path";
		if (error)
			*error = impl->error;
		return false;
	}
	if (databasePath == impl->config.databasePath || databasePath + ".merkle" == impl->config.databasePath)
		return Flush (error);
	try
	{
		impl->Submit (
				[this, databasePath]
				{
					sqlite3 *destination = nullptr;
					int		 status =
						sqlite3_open_v2 (databasePath.c_str (), &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
					try
					{
						SqlCheck (status, destination, "open saved world");
						sqlite3_busy_timeout (destination, 5000);
						SqlExec (destination, "PRAGMA synchronous=FULL");
						sqlite3_backup *backup = sqlite3_backup_init (destination, "main", impl->db, "main");
						if (!backup)
							throw std::runtime_error (sqlite3_errmsg (destination));
						status = sqlite3_backup_step (backup, -1);
						int finish = sqlite3_backup_finish (backup);
						SqlCheck (status, destination, "copy saved world");
						SqlCheck (finish, destination, "finish saved world");
						Statement journal (destination, "PRAGMA journal_mode=WAL");
						SqlCheck (sqlite3_step (journal.statement), destination, "enable saved world WAL");
						const char *mode = reinterpret_cast<const char *> (sqlite3_column_text (journal.statement, 0));
						if (!mode || std::string (mode) != "wal")
							throw std::runtime_error ("saved world database does not support WAL");
					}
					catch (...)
					{
						if (destination)
							sqlite3_close (destination);
						throw;
					}
					sqlite3_close (impl->db);
					impl->db = destination;
				})
			.get ();
		impl->config.databasePath = databasePath;
		return true;
	}
	catch (const std::exception &e)
	{
		impl->error = e.what ();
		if (error)
			*error = impl->error;
		return false;
	}
}

#include "world_merkle_api.inc"

WorldStats World::Stats () const
{
	WorldStats stats;
	stats.residentChunks = impl->resident.size ();
	stats.pendingChunks = impl->pending.size ();
	stats.pendingMeshes = impl->meshes.size ();
	stats.collisionPatches = impl->collisionPatches.size ();
	stats.collisionBytes = impl->collisionBytes;
	stats.collisionBuilds = impl->collisionBuilds;
	stats.collisionSyncBuilds = impl->collisionSyncBuilds;
	stats.collisionWorkerBuilds = impl->collisionWorkerBuilds;
	stats.collisionPending = impl->collisionPending.size ();
	stats.collisionPendingBytes = impl->collisionPendingBytes;
	stats.collisionStaleResults = impl->collisionStaleResults;
	stats.pendingEdits = impl->edits.size ();
	stats.pendingEditBytes = impl->pendingEditBytes;
	stats.pendingEntityPages = impl->pageJobs.size ();
	stats.pendingEntityPageBytes = impl->pendingPageBytes;
	for (const auto &[key, entry] : impl->resident)
	{
		stats.octreeNodes += entry.chunk->cells.NodeCount ();
		stats.residentBytes += entry.treeBytes;
		if (entry.mesh)
		{
			stats.meshQuads += entry.mesh->quads.size ();
			stats.meshTriangles += entry.mesh->indices.size () / 3;
			stats.meshBytes += entry.mesh->Bytes ();
		}
	}
	return stats;
}

bool World::Install (const Chunk &chunk, std::string *error)
{
	if (impl->edits.contains (chunk.key))
	{
		impl->error = "chunk has a pending durable edit";
		if (error)
			*error = impl->error;
		return false;
	}
	if (chunk.revision > static_cast<uint64_t> (INT64_MAX))
	{
		impl->error = "invalid network chunk revision";
		if (error)
			*error = impl->error;
		return false;
	}
	auto found = impl->resident.find (chunk.key);
	if (found != impl->resident.end () && found->second.chunk->revision > chunk.revision)
		return true;
	// Cancel publication of a queued generated chunk, while its independent worker
	// future is free to finish. Otherwise a late load could overwrite this revision.
	impl->pending.erase (chunk.key);
	size_t bytes = ChunkResidentBytes (chunk);
	if (!impl->CanAdmit (chunk, bytes))
	{
		if (error)
			*error = impl->error;
		return false;
	}
	auto replacement = std::make_shared<Chunk> (chunk);
	if (found == impl->resident.end ())
		impl->resident[chunk.key] = {std::move (replacement), nullptr, ++impl->clock, ++impl->serial, bytes};
	else
	{
		found->second.chunk = std::move (replacement);
		found->second.touched = ++impl->clock;
		found->second.treeBytes = bytes;
	}
	impl->Invalidate (chunk.key);
	impl->Trim ();
	if (!impl->resident.contains (chunk.key))
	{
		impl->error = "network chunk exceeds resident budget";
		if (error)
			*error = impl->error;
		return false;
	}
	return true;
}

TraceResult
World::Trace (const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs) const
{
	return TraceInternal (position, offset, mins, maxs, impl->config.generator != Generator::Empty);
}

TraceResult World::TraceVoxels (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs) const
{
	return TraceInternal (position, offset, mins, maxs, false);
}

bool World::CollisionPatches (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
	std::vector<CollisionPatchSnapshot> &output) const
{
	return CollisionPatchesInternal (position, offset, mins, maxs, output, false);
}
bool World::CollisionPatchesAsync (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
	std::vector<CollisionPatchSnapshot> &output) const
{
	impl->PumpCollision ();
	return CollisionPatchesInternal (position, offset, mins, maxs, output, true);
}
std::optional<uint64_t> World::CollisionSerial (ChunkKey key) const
{
	auto entry = impl->resident.find (CanonicalWorldKey (impl->config, key));
	return entry == impl->resident.end () ? std::nullopt : std::optional<uint64_t> (entry->second.meshSerial);
}
bool World::CollisionPatchesInternal (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs,
	std::vector<CollisionPatchSnapshot> &output, bool asynchronous) const
{
	output.clear ();
	WorldPosition start = position, end;
	if (!Normalize (start) || !Add (start, offset, end))
		return false;
	constexpr int		   patchSize = Impl::CollisionCells * VoxelSize;
	std::array<double, 3>  low, high;
	std::array<int64_t, 3> first, last;
	size_t				   chunkCount = 1, bytes = 0;
	for (int a = 0; a < 3; ++a)
	{
		if (!std::isfinite (mins[a]) || !std::isfinite (maxs[a]) || mins[a] > maxs[a] || !std::isfinite (offset[a]) || std::abs (offset[a]) > 1048576)
			return false;
		low[a] = start.local[a] + std::min (0.0, offset[a]) + mins[a];
		high[a] = start.local[a] + std::max (0.0, offset[a]) + maxs[a];
		const double lo = std::floor ((low[a] - VoxelSize) / ChunkSize);
		const double hi = std::floor ((high[a] + VoxelSize) / ChunkSize);
		if (lo < -4096 || hi > 4096 || hi - lo + 1 > 4096)
			return false;
		first[a] = int64_t (lo);
		last[a] = int64_t (hi);
		chunkCount *= size_t (last[a] - first[a] + 1);
		if (chunkCount > 4096)
			return false;
	}
	for (int64_t z = first[2]; z <= last[2]; ++z)
		for (int64_t y = first[1]; y <= last[1]; ++y)
			for (int64_t x = first[0]; x <= last[0]; ++x)
			{
				ChunkKey key;
				if (!CheckedAdd (start.chunk.x, x, key.x) || !CheckedAdd (start.chunk.y, y, key.y) || !CheckedAdd (start.chunk.z, z, key.z))
				{
					output.clear ();
					return false;
				}
				const std::array<double, 3> chunkMin{double (x) * ChunkSize, double (y) * ChunkSize, double (z) * ChunkSize};
				std::array<int, 3>			from, to;
				for (int a = 0; a < 3; ++a)
				{
					from[a] = std::max (0, int (std::floor ((low[a] - chunkMin[a] - 4 - VoxelSize) / patchSize)));
					to[a] = std::min (VoxelsPerChunk / Impl::CollisionCells - 1, int (std::floor ((high[a] - chunkMin[a] - 4 + VoxelSize) / patchSize)));
				}
				const bool unknown = !Find (key);
				for (int pz = from[2]; pz <= to[2]; ++pz)
					for (int py = from[1]; py <= to[1]; ++py)
						for (int px = from[0]; px <= to[0]; ++px)
						{
							const std::array<int, 3> patch{px, py, pz};
							double					 enter = 0, leave = 1;
							for (int a = 0; a < 3; ++a)
							{
								const double lo = chunkMin[a] + patch[a] * patchSize - VoxelSize - maxs[a];
								const double hi = chunkMin[a] + (patch[a] + 1) * patchSize + VoxelSize - mins[a];
								if (!offset[a])
								{
									if (start.local[a] < lo || start.local[a] > hi)
										leave = -1;
									continue;
								}
								double t0 = (lo - start.local[a]) / offset[a], t1 = (hi - start.local[a]) / offset[a];
								if (t0 > t1)
									std::swap (t0, t1);
								enter = std::max (enter, t0);
								leave = std::min (leave, t1);
							}
							if (enter > leave)
								continue;
							if (output.size () >= 4096)
							{
								output.clear ();
								return false;
							}
							auto mesh = unknown ? nullptr : impl->CollisionMesh (key, patch, asynchronous);
							if (mesh)
								bytes += mesh->Bytes ();
							if (bytes > Impl::CollisionBudget)
							{
								output.clear ();
								return false;
							}
							const auto serial = CollisionSerial (key);
							output.push_back (
								{CanonicalWorldKey (impl->config, key), patch, serial.value_or (0), mesh, unknown || (!asynchronous && !mesh),
								 asynchronous && !unknown && !mesh});
						}
			}
	return true;
}

Cell World::CollisionContentsCached (const WorldPosition &position) const
{
	WorldPosition canonical = position;
	if (!Normalize (canonical))
		return {0, Unknown};
	canonical.chunk = CanonicalWorldKey (impl->config, canonical.chunk);
	const auto chunk = Find (canonical.chunk);
	if (!chunk)
		return {0, Unknown};
	const Cell stored = Sample (canonical);
	if (impl->config.generator == Generator::Empty && (!chunk->solids || chunk->solids->empty ()))
		return stored;
	std::array<int, 3> patch;
	for (int a = 0; a < 3; ++a)
		patch[a] = int (canonical.local[a]) / (Impl::CollisionCells * VoxelSize);
	impl->PumpCollision (4);
	if (!impl->CollisionMesh (canonical.chunk, patch, true))
		return {0, Unknown};
	const Impl::CollisionId id{canonical.chunk.x, canonical.chunk.y, canonical.chunk.z, patch[0], patch[1], patch[2]};
	const auto			   &density = impl->collisionPatches.at (id).density;
	if (density.empty ())
		return stored;
	std::array<int, 3>	  cell, order{0, 1, 2};
	std::array<double, 3> fraction;
	for (int a = 0; a < 3; ++a)
	{
		const double lattice = (canonical.local[a] - patch[a] * Impl::CollisionCells * VoxelSize + VoxelSize / 2) / VoxelSize;
		cell[a] = int (std::floor (lattice));
		fraction[a] = lattice - cell[a];
	}
	std::sort (order.begin (), order.end (), [&] (int a, int b) { return fraction[a] > fraction[b]; });
	auto value = [&] (int corner)
	{
		return double (
			density
				[((cell[2] + ((corner >> 2) & 1)) * Impl::CollisionSamples + cell[1] + ((corner >> 1) & 1)) * Impl::CollisionSamples + cell[0] + (corner & 1)]);
	};
	// Same Freudenthal tetrahedra as marching tetrahedra. Feature extraction
	// consumes this lattice too; its triangles remain the authoritative hull.
	const int	 first = 1 << order[0], second = first | (1 << order[1]);
	const double signedDensity = value (0) * (1 - fraction[order[0]]) + value (first) * (fraction[order[0]] - fraction[order[1]]) +
								 value (second) * (fraction[order[1]] - fraction[order[2]]) + value (7) * fraction[order[2]];
	return signedDensity > 0 ? Cell{uint16_t (stored.material ? stored.material : 1), Solid} : (stored.IsSolid () ? Cell{} : stored);
}

Cell World::CollisionContents (const WorldPosition &position) const
{
	WorldPosition canonical = position;
	if (!Normalize (canonical))
		return {0, Unknown};
	Cell stored = Sample (canonical);
	auto chunk = Find (canonical.chunk);
	if (stored.contents == Unknown || (impl->config.generator == Generator::Empty && (!chunk || !chunk->solids || chunk->solids->empty ())))
		return stored;
	auto trace = Trace (canonical, {});
	if (trace.startSolid)
		return trace.cell;
	return stored.IsSolid () ? Cell{} : stored;
}

TraceResult World::TraceInternal (
	const WorldPosition &position, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs, bool smooth) const
{
	TraceResult result;
	result.end = position;
	WorldPosition start = position;
	auto		  blocked = [&]
	{
		result.fraction = 0;
		result.startSolid = result.allSolid = result.hitUnknown = true;
		result.cell = {0, Unknown};
		return result;
	};
	if (!Normalize (start) || !Add (start, offset, result.end))
		return blocked ();
	std::array<double, 3>  sweepMin, sweepMax;
	std::array<int64_t, 3> first, last;
	size_t				   chunkCount = 1;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite (mins[axis]) || !std::isfinite (maxs[axis]) || mins[axis] > maxs[axis] || std::abs (offset[axis]) > 1048576)
			return blocked ();
		sweepMin[axis] = start.local[axis] + std::min (0.0, offset[axis]) + mins[axis];
		sweepMax[axis] = start.local[axis] + std::max (0.0, offset[axis]) + maxs[axis];
		double lo = std::floor ((sweepMin[axis] - VoxelSize) / ChunkSize);
		double hi = std::floor ((sweepMax[axis] + VoxelSize) / ChunkSize);
		if (lo < -4096 || hi > 4096 || hi - lo + 1 > 4096)
			return blocked ();
		first[axis] = static_cast<int64_t> (lo);
		last[axis] = static_cast<int64_t> (hi); // Feature vertices can extend into either neighboring chunk.
		chunkCount *= static_cast<size_t> (last[axis] - first[axis] + 1);
		if (chunkCount > 4096)
			return blocked ();
	}
	std::vector<std::pair<double, double>> intervals;
	std::vector<std::pair<double, bool>>   featureCrossings;
	bool								   featureCenterInside = false;
	double								   earliest = 1.0;
	bool								   hit = false;
	size_t								   visitedRanges = 0;
	bool								   exhausted = false;
	const std::array<double, 3>			   localStart{start.local[0], start.local[1], start.local[2]};
	auto								   intersects = [&] (const std::array<double, 3> &low, const std::array<double, 3> &high)
	{
		double enter = 0, leave = 1;
		for (int axis = 0; axis < 3; ++axis)
		{
			const double lo = low[axis] - maxs[axis], hi = high[axis] - mins[axis];
			if (!offset[axis])
			{
				if (localStart[axis] < lo || localStart[axis] > hi)
					return false;
				continue;
			}
			double a = (lo - localStart[axis]) / offset[axis], b = (hi - localStart[axis]) / offset[axis];
			if (a > b)
				std::swap (a, b);
			enter = std::max (enter, a);
			leave = std::min (leave, b);
			if (enter > leave)
				return false;
		}
		return true;
	};
	auto box = [&] (const std::array<double, 3> &boxMin, const std::array<double, 3> &boxMax, Cell cell)
	{
		double enter = -std::numeric_limits<double>::infinity ();
		double leave = std::numeric_limits<double>::infinity ();
		int	   normalAxis = -1;
		float  normalSign = 0;
		bool   inside = true;
		for (int axis = 0; axis < 3; ++axis)
		{
			double lo = boxMin[axis] - maxs[axis];
			double hi = boxMax[axis] - mins[axis];
			double origin = start.local[axis];
			// Rays have zero extent: include their boundary so an axis-aligned
			// shot cannot slip between two solid leaves at an octree split.
			// Boxes use strict overlap to allow sliding along a touched wall.
			bool   pointAxis = mins[axis] == maxs[axis];
			inside = inside && (pointAxis ? origin >= lo && origin <= hi : origin > lo && origin < hi);
			if (offset[axis] == 0)
			{
				// Merely resting against a face is not penetration and must permit
				// sliding along the floor, walls, and adjacent coplanar leaf faces.
				if (pointAxis ? origin < lo || origin > hi : origin <= lo || origin >= hi)
					return;
				continue;
			}
			double a = (lo - origin) / offset[axis];
			double b = (hi - origin) / offset[axis];
			float  sign = -1;
			if (a > b)
			{
				std::swap (a, b);
				sign = 1;
			}
			if (a > enter)
			{
				enter = a;
				normalAxis = axis;
				normalSign = sign;
			}
			leave = std::min (leave, b);
			if (enter >= leave)
				return;
		}
		if (leave <= 0 || enter > 1)
			return;
		if (inside)
		{
			result.startSolid = true;
			result.hitUnknown = result.hitUnknown || cell.contents == Unknown;
			result.cell = cell;
		}
		intervals.emplace_back (std::max (0.0, enter), std::min (1.0, leave));
		if (enter >= 0 && enter <= earliest && normalAxis >= 0)
		{
			earliest = enter;
			hit = true;
			result.normal = {};
			result.normal[normalAxis] = normalSign;
			result.cell = cell;
			result.hitUnknown = cell.contents == Unknown;
		}
	};
	for (int64_t x = first[0]; x <= last[0]; ++x)
		for (int64_t y = first[1]; y <= last[1]; ++y)
			for (int64_t z = first[2]; z <= last[2]; ++z)
			{
				std::array<int64_t, 3> delta = {x, y, z};
				ChunkKey			   key;
				bool				   valid = true;
				std::array<double, 3>  chunkMin, chunkMax;
				for (int axis = 0; axis < 3; ++axis)
				{
					valid = CheckedAdd (start.chunk[axis], delta[axis], key[axis]) && valid;
					chunkMin[axis] = static_cast<double> (delta[axis]) * ChunkSize;
					chunkMax[axis] = chunkMin[axis] + ChunkSize;
				}
				auto chunk = valid ? Find (key) : nullptr;
				if (!chunk)
				{
					box (chunkMin, chunkMax, {0, Unknown});
					continue;
				}

				if (impl->config.generator == Generator::Planet || (chunk->solids && !chunk->solids->empty ()))
				{
					SolidPoint		   relative = localStart;
					std::array<int, 3> patchLow, patchHigh;
					constexpr int	   patchSize = Impl::CollisionCells * VoxelSize;
					constexpr int	   patchCount = VoxelsPerChunk / Impl::CollisionCells;
					for (int a = 0; a < 3; ++a)
					{
						relative[a] -= chunkMin[a];
						// QEF vertices belong to cells on the shifted sample lattice;
						// owned triangles may extend one cell beyond their grid edge.
						patchLow[a] = std::max (0, int (std::floor ((sweepMin[a] - chunkMin[a] - 4 - VoxelSize) / patchSize)));
						patchHigh[a] = std::min (patchCount - 1, int (std::floor ((sweepMax[a] - chunkMin[a] - 4 + VoxelSize) / patchSize)));
					}
					for (int pz = patchLow[2]; pz <= patchHigh[2]; ++pz)
						for (int py = patchLow[1]; py <= patchHigh[1]; ++py)
							for (int px = patchLow[0]; px <= patchHigh[0]; ++px)
							{
								const std::array<int, 3> patch{px, py, pz};
								std::array<double, 3>	 low, high;
								for (int a = 0; a < 3; ++a)
								{
									low[a] = chunkMin[a] + 4 + patch[a] * patchSize - VoxelSize;
									high[a] = chunkMin[a] + 4 + (patch[a] + 1) * patchSize;
								}
								if (!intersects (low, high))
									continue;
								if (++visitedRanges > 16384)
									return blocked ();
								auto mesh = impl->CollisionMesh (key, patch);
								if (!mesh)
								{
									box (low, high, {0, Unknown});
									continue;
								}
								auto contact = SweepFeatureMesh (*mesh, relative, offset, {mins[0], mins[1], mins[2]}, {maxs[0], maxs[1], maxs[2]});
								if (contact.hit && contact.fraction < earliest)
								{
									earliest = contact.fraction;
									hit = true;
									result.normal = {float (contact.normal[0]), float (contact.normal[1]), float (contact.normal[2])};
									result.cell = {1, Solid};
								}
								result.startSolid |= contact.startSolid;
								intervals.insert (intervals.end (), contact.surfaceIntervals.begin (), contact.surfaceIntervals.end ());
								featureCrossings.insert (featureCrossings.end (), contact.centerCrossings.begin (), contact.centerCrossings.end ());
							}
					// The surface overlaps above detect partially embedded hulls.
					// Classify the center as well to include hulls wholly inside the
					// solid; oriented mesh crossings determine where they leave it.
					for (int a = 0; a < 3; ++a)
						relative[a] += (double (mins[a]) + maxs[a]) * 0.5;
					if (relative[0] >= 0 && relative[0] < ChunkSize && relative[1] >= 0 && relative[1] < ChunkSize && relative[2] >= 0 &&
						relative[2] < ChunkSize)
					{
						TerrainNeighborhood neighborhood{};
						neighborhood[13] = chunk;
						featureCenterInside |= SamplePlanetSurface (impl->config, neighborhood, relative).density > 0;
					}
					continue;
				}
				if (smooth)
				{
					if (!intersects (chunkMin, chunkMax))
						continue;
					TerrainNeighborhood neighbors{};
					for (int dz = -1; dz <= 1; ++dz)
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								ChunkKey adjacent;
								if (CheckedAdd (key.x, dx, adjacent.x) && CheckedAdd (key.y, dy, adjacent.y) && CheckedAdd (key.z, dz, adjacent.z))
									neighbors[(dz + 1) * 9 + (dy + 1) * 3 + dx + 1] = Find (adjacent);
							}
					// Small direct-mapped caches bound scratch memory independently of
					// trace length. Unlike the render worker's rolling slabs, they permit
					// arbitrary near/far traversal order without stale density samples.
					struct CachedSample
					{
						uint32_t			 tag = UINT32_MAX;
						TerrainDensitySample value;
					};
					struct CachedColumn
					{
						uint32_t			 tag = UINT32_MAX;
						TerrainDensityColumn value;
					};
					std::array<CachedSample, 512> sampleCache;
					std::array<CachedColumn, 256> columnCache;
					auto						  sample = [&] (TerrainDensityPoint point)
					{
						const uint32_t columnId = uint32_t ((point[1] + 1) * 130 + point[0] + 1);
						const uint32_t sampleId = uint32_t (point[2] + 1) * 130 * 130 + columnId;
						auto		  &cached = sampleCache[(sampleId * 2654435761u) >> 23];
						if (cached.tag != sampleId)
						{
							auto &column = columnCache[(columnId * 2654435761u) >> 24];
							if (column.tag != columnId)
							{
								column.tag = columnId;
								column.value = TerrainSampleColumn (impl->config, key, point[0], point[1]);
							}
							cached.tag = sampleId;
							cached.value = TerrainSampleDensity (impl->config, key, point, neighbors, column.value);
						}
						return cached.value;
					};
					auto uniform = [&] (TerrainDensityPoint low, TerrainDensityPoint high) -> std::optional<Cell>
					{
						// A range contains the sample at its upper cube endpoint too.
						// Only use a homogeneous leaf when every sample belongs to it.
						for (int axis = 0; axis < 3; ++axis)
							if (low[axis] < 0 || high[axis] >= VoxelsPerChunk)
								return std::nullopt;
						// Rounded voxel occupancy can differ from density in this band.
						if (impl->config.generator == Generator::Planet)
							return std::nullopt;
						if (key.z == 0 && low[2] * VoxelSize + VoxelSize / 2 <= 128 && high[2] * VoxelSize + VoxelSize / 2 >= 64)
							return std::nullopt;
						const Node *node = &chunk->cells.impl->root;
						for (int size = VoxelsPerChunk; node->children; size /= 2)
						{
							const int half = size / 2;
							int		  child = 0;
							for (int axis = 0; axis < 3; ++axis)
							{
								if ((low[axis] >= half) != (high[axis] >= half))
									return std::nullopt;
								if (low[axis] >= half)
								{
									child |= 1 << axis;
									low[axis] -= half;
									high[axis] -= half;
								}
							}
							node = &(*node->children)[child];
						}
						return node->cell;
					};
					auto visit = [&] (auto &&self, TerrainDensityPoint low, TerrainDensityPoint high) -> void
					{
						if (exhausted)
							return;
						if (++visitedRanges > 262144)
						{
							exhausted = true;
							return;
						}
						std::array<double, 3> rangeMin, rangeMax;
						for (int axis = 0; axis < 3; ++axis)
						{
							rangeMin[axis] = chunkMin[axis] + low[axis] * VoxelSize + VoxelSize / 2;
							rangeMax[axis] = chunkMin[axis] + high[axis] * VoxelSize + VoxelSize / 2;
						}
						if (!intersects (rangeMin, rangeMax))
							return;
						if (auto cell = uniform (low, high))
						{
							if (cell->IsSolid ())
								box (rangeMin, rangeMax, *cell);
							return;
						}
						int axis = 0;
						for (int a = 1; a < 3; ++a)
							if (high[a] - low[a] > high[axis] - low[axis])
								axis = a;
						if (high[axis] - low[axis] > 1)
						{
							const int middle = (low[axis] + high[axis]) / 2;
							auto	  lowerHigh = high, upperLow = low;
							lowerHigh[axis] = upperLow[axis] = middle;
							self (self, low, lowerHigh);
							self (self, upperLow, high);
							return;
						}
						std::array<TerrainDensitySample, 8> samples;
						for (int i = 0; i < 8; ++i)
							samples[i] = sample ({low[0] + (i & 1), low[1] + ((i >> 1) & 1), low[2] + ((i >> 2) & 1)});
						const auto contacts = TraceTerrainCube (samples, rangeMin, localStart, offset, mins, maxs);
						for (size_t i = 0; i < contacts.count; ++i)
						{
							const auto &contact = contacts.intervals[i];
							if (contact.startSolid)
							{
								result.startSolid = true;
								result.cell = contact.cell;
								result.hitUnknown |= contact.cell.contents == Unknown;
							}
							intervals.emplace_back (std::max (0.0, contact.enter), std::min (1.0, contact.exit));
							if (contact.enter >= 0 && contact.enter <= earliest)
							{
								earliest = contact.enter;
								hit = true;
								result.normal = contact.normal;
								result.cell = contact.cell;
								result.hitUnknown = contact.cell.contents == Unknown;
							}
						}
					};
					TerrainDensityPoint low, high;
					for (int axis = 0; axis < 3; ++axis)
					{
						low[axis] = std::max (-1, int (std::floor ((sweepMin[axis] - chunkMin[axis] - VoxelSize / 2) / VoxelSize)));
						high[axis] = std::min (VoxelsPerChunk, int (std::floor ((sweepMax[axis] - chunkMin[axis] - VoxelSize / 2) / VoxelSize)) + 1);
					}
					visit (visit, low, high);
					if (exhausted)
						return blocked ();
					continue;
				}
				std::function<void (const Node &, const std::array<double, 3> &, double)> visit;
				visit = [&] (const Node &node, const std::array<double, 3> &nodeMin, double size)
				{
					std::array<double, 3> nodeMax;
					for (int axis = 0; axis < 3; ++axis)
					{
						nodeMax[axis] = nodeMin[axis] + size;
						if (nodeMax[axis] < sweepMin[axis] || nodeMin[axis] > sweepMax[axis])
							return;
					}
					if (!node.children)
					{
						if (node.cell.IsSolid ())
							box (nodeMin, nodeMax, node.cell);
						return;
					}
					for (int i = 0; i < 8; ++i)
					{
						auto childMin = nodeMin;
						for (int axis = 0; axis < 3; ++axis)
							childMin[axis] += ((i >> axis) & 1) * size / 2;
						visit ((*node.children)[i], childMin, size / 2);
					}
				};
				visit (chunk->cells.impl->root, chunkMin, ChunkSize);
			}
	// Merge center crossings from all chunk-owned triangles. Several triangles
	// can report the same edge/vertex hit; apply that transition only once.
	std::sort (featureCrossings.begin (), featureCrossings.end ());
	bool   featureInside = featureCenterInside;
	double featureBegin = 0;
	for (size_t i = 0; i < featureCrossings.size ();)
	{
		const double time = featureCrossings[i].first;
		bool		 entry = false, exit = false;
		do
		{
			entry |= featureCrossings[i].second;
			exit |= !featureCrossings[i].second;
			++i;
		} while (i < featureCrossings.size () && featureCrossings[i].first <= time + 1e-8);
		if (entry == exit)
			continue; // Tangential contact at an edge does not change occupancy.
		if (exit && featureInside)
		{
			intervals.emplace_back (featureBegin, time);
			featureInside = false;
		}
		else if (entry && !featureInside)
		{
			featureBegin = time;
			featureInside = true;
		}
	}
	if (featureInside)
		intervals.emplace_back (featureBegin, 1);
	result.startSolid |= featureCenterInside;
	if (result.startSolid && result.cell.contents == Empty)
		result.cell = {1, Solid};

	if (result.startSolid)
	{
		result.fraction = 0;
		result.end = start;
		std::sort (intervals.begin (), intervals.end ());
		double covered = 0;
		for (const auto &interval : intervals)
		{
			if (interval.first > covered + 1e-10)
				break;
			covered = std::max (covered, interval.second);
		}
		result.allSolid = covered >= 1;
		return result;
	}
	if (hit)
	{
		double speed = 0;
		for (int axis = 0; axis < 3; ++axis)
			speed -= offset[axis] * result.normal[axis];
		result.fraction = std::max (0.0, earliest - CollisionEpsilon / std::max (speed, 1e-12));
		std::array<double, 3> travelled;
		for (int axis = 0; axis < 3; ++axis)
			travelled[axis] = offset[axis] * result.fraction;
		if (!Add (start, travelled, result.end))
			return blocked ();
	}
	return result;
}
} // namespace quiver
