/*
 * Quiver streamed voxel rendering. GPL-2.0-or-later.
 * Mesh jobs produce immutable chunk-local snapshots. GPU buffers are rebuilt
 * only for a changed mesh serial and translated relative to the camera frame.
 */
#include "quakedef.h"
#include "octree_engine.h"
#include "octree_render.h"
#include "terrain_lod_cache.h"
#include "terrain_materials.h"
#include "terrain_textures.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

extern cvar_t r_sun_angle, r_sun_azimuth, r_sun_intensity, r_showtris;

namespace
{
cvar_t r_octree_lod = {"r_octree_lod", "1", CVAR_ARCHIVE};
cvar_t r_octree_lod_distance = {"r_octree_lod_distance", "32768", CVAR_ARCHIVE};
cvar_t r_octree_distance = {"r_octree_distance", "4096", CVAR_ARCHIVE};
cvar_t r_octree_mesh_mb = {"r_octree_mesh_mb", "128", CVAR_ARCHIVE};
cvar_t r_octree_upload_mb = {"r_octree_upload_mb", "8", CVAR_ARCHIVE};
cvar_t r_octree_smooth = {"r_octree_smooth", "1", CVAR_ARCHIVE};

struct KeyLess
{
	bool operator() (const quiver::ChunkKey &a, const quiver::ChunkKey &b) const
	{
		if (a.x != b.x)
			return a.x < b.x;
		if (a.y != b.y)
			return a.y < b.y;
		return a.z < b.z;
	}
};

struct Vertex
{
	float position[3];
	float normal[3];
	byte  weights[4];
	byte  tint[4];
};
static_assert (sizeof (Vertex) == 32);
static_assert (offsetof (Vertex, normal) == 12 && offsetof (Vertex, weights) == 24 && offsetof (Vertex, tint) == 28);

struct PreparedChunk
{
	quiver::ChunkKey						 key;
	std::shared_ptr<const quiver::ChunkMesh> mesh;
	std::array<double, 3>					 translation;
	double									 distanceSquared;
};

struct PreparedTile
{
	quiver::TerrainTileKey						   key;
	std::shared_ptr<const quiver::TerrainTileMesh> mesh;
	std::array<double, 3>						   translation;
	double										   distanceSquared;
};

struct PreparedFrame
{
	std::vector<PreparedChunk>	 chunks;
	std::vector<PreparedTile>	 tiles;
	quiver::TerrainLodCacheStats cacheStats;
	quiver::WorldPosition		 origin;
	quiver::WorldConfig			 config;
	uint64_t					 seed = 1;
	uint64_t					 terrainGeneration = 0;
	bool						 proceduralTerrain = false;
	std::array<float, 4>		 sunlight{0, 0.70710678f, 0.70710678f, 1};
	size_t						 memoryBudget = 0;
	size_t						 uploadBudget = 0;
};

struct GpuMesh
{
	VkBuffer				buffer = VK_NULL_HANDLE;
	vulkan_memory_t			memory{};
	VkDeviceSize			indexOffset = 0;
	uint32_t				indexCount = 0;
	uint32_t				oceanIndexCount = 0;
	std::array<uint32_t, 4> transitionCounts{};
	uint64_t				serial = 0;
	size_t					bytes = 0;
	bool					terrain = false;
};

struct RetiredMesh
{
	GpuMesh	 mesh;
	uint64_t frame;
};

// The viewmodel/showtris task depends on the world task when showtris is
// active. Buffers are already retained across in-flight frames by RetireMesh.
struct WireDraw
{
	VkBuffer				 buffer;
	VkDeviceSize			 indexOffset;
	uint32_t				 first, count;
	std::array<float, 16>	 matrix;
	std::array<float, 4>	 curvature;
};
std::vector<WireDraw> wireDraws;

std::mutex									 preparedMutex;
std::shared_ptr<const PreparedFrame>		 preparedFrame;
std::map<quiver::ChunkKey, GpuMesh, KeyLess> meshes;
struct GpuTerrain
{
	GpuMesh				 mesh;
	std::array<float, 3> mins, maxs;
	float				 surfaceMin = 0, surfaceMax = 0;
	uint64_t			 generation = 0;
};
std::map<quiver::TerrainTileKey, GpuTerrain, quiver::TerrainTileLess> terrainMeshes;
std::unique_ptr<quiver::TerrainLodCache>							  terrainCache;
uint64_t															  terrainSeed = 0;
bool																  terrainSmooth = true;
quiver::Generator													  terrainGenerator = quiver::Generator::Terrain;
std::weak_ptr<quiver::PlanetGenerator>								  terrainPlanet;
uint64_t															  terrainGeneration = 0;
struct RenderStats
{
	size_t						 nearDraws = 0, nearTriangles = 0, farDraws = 0, farTriangles = 0;
	size_t						 gpuBytes = 0, uploadedBytes = 0, retiredBytes = 0;
	quiver::TerrainLodCacheStats cache;
};
std::mutex					 statsMutex;
RenderStats					 renderStats;
std::vector<RetiredMesh>	 retired;
std::atomic<uint64_t>		 frameSerial{0};
std::array<gltexture_t *, 4> materialTextures{};
size_t						 allocatedBytes = 0;
size_t						 nearAllocatedBytes = 0;

void FreeMesh (GpuMesh &mesh)
{
	R_FreeBuffer (mesh.buffer, &mesh.memory, nullptr);
	allocatedBytes -= mesh.bytes;
	if (!mesh.terrain)
		nearAllocatedBytes -= mesh.bytes;
	mesh = {};
}

void RetireMesh (GpuMesh &mesh, uint64_t frame)
{
	if (mesh.buffer != VK_NULL_HANDLE)
		retired.push_back ({mesh, frame});
	mesh = {};
}

void CollectMeshes (uint64_t frame)
{
	// The renderer has two frames in flight. Three fence advances also cover a
	// mesh last referenced by the preceding frame when it was retired.
	for (auto it = retired.begin (); it != retired.end ();)
	{
		if (frame - it->frame >= 3)
		{
			FreeMesh (it->mesh);
			it = retired.erase (it);
		}
		else
			++it;
	}
}

void CreateMaterialTexture ()
{
	if (materialTextures[0])
		return;

	// Retain source pixels for texture-manager reloads, including vid_restart.
	// Separate images keep every mip level free of neighboring atlas materials.
	static std::array<quiver::TerrainTexturePixels, 4> pixels;
	static constexpr const char						  *names[] = {"octree:grass", "octree:soil", "octree:rock", "octree:sand"};
	for (size_t i = 0; i < materialTextures.size (); ++i)
	{
		if (pixels[i].empty ())
			pixels[i] = quiver::BuildTerrainTexture (static_cast<quiver::TerrainTextureKind> (i));
		// Mip generation downsamples its input in place. Reload sources must
		// remain pristine even after repeated map changes and video restarts.
		auto uploadPixels = pixels[i];
		materialTextures[i] = TexMgr_LoadImage (
			nullptr, names[i], quiver::TerrainTextureSize, quiver::TerrainTextureSize, SRC_RGBA, uploadPixels.data (), "",
			reinterpret_cast<src_offset_t> (pixels[i].data ()), TEXPREF_MIPMAP | TEXPREF_PERSIST);
	}
}

struct MeshAllocation
{
	size_t vertices = 0;
	size_t indices = 0;
	size_t Bytes () const
	{
		return vertices * sizeof (Vertex) + indices * sizeof (uint32_t);
	}
};

int QuadSteps (const quiver::MeshQuad &quad)
{
	return std::max (1, static_cast<int> (quad.size / quiver::VoxelSize));
}

MeshAllocation MeasureMesh (const quiver::ChunkMesh &source)
{
	if (source.smooth)
		return {source.vertices.size () + source.transitionVertices.size (), source.indices.size () + source.transitionIndices.size ()};
	MeshAllocation allocation;
	for (const auto &quad : source.quads)
	{
		const int steps = QuadSteps (quad);
		allocation.vertices += steps == 1 ? 4 : steps * 4 + 1;
		allocation.indices += steps == 1 ? 6 : steps * 12;
	}
	return allocation;
}

bool IsProceduralTop (const quiver::MeshQuad &quad, quiver::ChunkKey key, uint64_t seed, bool procedural)
{
	if (!procedural || key.z != 0 || quad.axis != 2 || quad.sign <= 0)
		return false;
	// Preserve flat normals on edits, cave floors and raised blocks. A merged
	// generated top must match its original height at the center and corners.
	const float last = std::max (0.0f, quad.size - quiver::VoxelSize);
	const float samples[5][2] = {{0, 0}, {last, 0}, {0, last}, {last, last}, {quad.size * 0.5f, quad.size * 0.5f}};
	for (const auto &sample : samples)
	{
		const int	x = std::clamp (int (std::floor ((quad.origin[0] + sample[0]) / quiver::VoxelSize)), 0, quiver::VoxelsPerChunk - 1);
		const int	y = std::clamp (int (std::floor ((quad.origin[1] + sample[1]) / quiver::VoxelSize)), 0, quiver::VoxelsPerChunk - 1);
		const float height = float (quiver::VoxelSize * quiver::TerrainHeightVoxels (seed, key.x, key.y, x, y));
		if (std::abs (quad.origin[2] - height) > 0.01f)
			return false;
	}
	return true;
}

GpuMesh UploadMesh (const quiver::ChunkMesh &source, uint64_t seed, bool procedural)
{
	GpuMesh mesh;
	mesh.serial = source.serial;
	const MeshAllocation allocation = MeasureMesh (source);
	if (!allocation.vertices || !allocation.indices)
		return mesh;
	mesh.indexOffset = allocation.vertices * sizeof (Vertex);
	mesh.indexCount = static_cast<uint32_t> (source.smooth ? source.indices.size () : allocation.indices);
	mesh.transitionCounts = source.transitionCounts;
	mesh.bytes = allocation.Bytes ();
	std::vector<Vertex>	  vertices (allocation.vertices);
	std::vector<uint32_t> indices (allocation.indices);
	uint32_t			  vertex = 0;
	size_t				  index = 0;
	if (source.smooth)
	{
		for (size_t i = 0; i < allocation.vertices; ++i)
		{
			const auto &input = i < source.vertices.size () ? source.vertices[i] : source.transitionVertices[i - source.vertices.size ()];
			auto	   &out = vertices[i];
			std::copy (input.position.begin (), input.position.end (), out.position);
			std::copy (input.normal.begin (), input.normal.end (), out.normal);
			std::copy (input.blend.weights.begin (), input.blend.weights.end (), out.weights);
			std::copy (input.blend.tint.begin (), input.blend.tint.end (), out.tint);
		}
		std::copy (source.indices.begin (), source.indices.end (), indices.begin ());
		for (size_t i = 0; i < source.transitionIndices.size (); ++i)
			indices[source.indices.size () + i] = uint32_t (source.vertices.size ()) + source.transitionIndices[i];
	}
	else
		for (const auto &quad : source.quads)
		{
			const int  tangentU = (quad.axis + 1) % 3;
			const int  tangentV = (quad.axis + 2) % 3;
			const auto blend = quiver::TerrainClassifyMaterial (quad.material, procedural);
			const bool smooth = IsProceduralTop (quad, source.key, seed, procedural);
			auto	   emitVertex = [&] (float u, float v)
			{
				Vertex &out = vertices[vertex++];
				std::copy (quad.origin.begin (), quad.origin.end (), out.position);
				out.position[tangentU] += u;
				out.position[tangentV] += v;
				std::copy (blend.weights.begin (), blend.weights.end (), out.weights);
				std::copy (blend.tint.begin (), blend.tint.end (), out.tint);
				std::fill (std::begin (out.normal), std::end (out.normal), 0.0f);
				out.normal[quad.axis] = float (quad.sign);
				if (smooth)
				{
					const auto normal = quiver::TerrainSurfaceNormal (seed, source.key.x, source.key.y, out.position[0], out.position[1]);
					std::copy (normal.begin (), normal.end (), out.normal);
				}
			};
			auto emitTriangle = [&] (uint32_t a, uint32_t b, uint32_t c)
			{
				// Match the shared BSP world pipeline: clockwise when viewed from outside.
				indices[index++] = a;
				indices[index++] = quad.sign > 0 ? c : b;
				indices[index++] = quad.sign > 0 ? b : c;
			};
			const uint32_t first = vertex;
			const int	   steps = QuadSteps (quad);
			if (steps == 1)
			{
				emitVertex (0, 0);
				emitVertex (quad.size, 0);
				emitVertex (quad.size, quad.size);
				emitVertex (0, quad.size);
				emitTriangle (first, first + 1, first + 2);
				emitTriangle (first, first + 2, first + 3);
			}
			else
			{
				// Preserve every finest-cell point on merged face perimeters. All
				// adjacent faces share identical edge vertices, including chunk seams.
				emitVertex (quad.size * 0.5f, quad.size * 0.5f);
				for (int side = 0; side < 4; ++side)
					for (int i = 0; i < steps; ++i)
					{
						const float t = i * quiver::VoxelSize;
						switch (side)
						{
						case 0:
							emitVertex (t, 0);
							break;
						case 1:
							emitVertex (quad.size, t);
							break;
						case 2:
							emitVertex (quad.size - t, quad.size);
							break;
						case 3:
							emitVertex (0, quad.size - t);
							break;
						}
					}
				for (int i = 0; i < steps * 4; ++i)
					emitTriangle (first, first + 1 + i, first + 1 + (i + 1) % (steps * 4));
			}
		}
	R_CreateBuffer (
		&mesh.buffer, &mesh.memory, mesh.bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, nullptr, nullptr, "octree chunk");
	std::vector<byte> storage (mesh.bytes);
	memcpy (storage.data (), vertices.data (), mesh.indexOffset);
	memcpy (storage.data () + mesh.indexOffset, indices.data (), indices.size () * sizeof (uint32_t));
	R_StagingUploadBuffer (mesh.buffer, mesh.bytes, storage.data ());
	allocatedBytes += mesh.bytes;
	nearAllocatedBytes += mesh.bytes;
	return mesh;
}

// Dyadic tile overlap is containment. Unsigned subtraction after ordering is
// exact even when the signed coordinate difference would overflow.
bool TileContains (const quiver::TerrainTileKey &outer, const quiver::TerrainTileKey &inner)
{
	const uint64_t span = uint64_t (1) << outer.level;
	return outer.level >= inner.level && outer.x <= inner.x && outer.y <= inner.y && uint64_t (inner.x) - uint64_t (outer.x) < span &&
		   uint64_t (inner.y) - uint64_t (outer.y) < span;
}
bool TilesOverlap (const quiver::TerrainTileKey &a, const quiver::TerrainTileKey &b)
{
	return TileContains (a, b) || TileContains (b, a);
}

GpuMesh UploadTerrain (const quiver::TerrainTileMesh &source)
{
	GpuMesh mesh;
	mesh.terrain = true;
	if (source.vertices.empty () || source.indices.empty ())
		return mesh;
	// Tile builders have a fixed grid. Reject corrupt oversized snapshots before
	// narrowing counts or allocating Vulkan memory.
	if (source.vertices.size () > 4096 || source.indices.size () > 16384)
		return mesh;
	std::vector<Vertex> vertices (source.vertices.size ());
	for (size_t i = 0; i < source.vertices.size (); ++i)
	{
		const auto &input = source.vertices[i];
		auto	   &out = vertices[i];
		std::copy (input.position.begin (), input.position.end (), out.position);
		const auto blend = quiver::TerrainClassifyMaterial (input.material, true);
		std::copy (blend.weights.begin (), blend.weights.end (), out.weights);
		std::copy (blend.tint.begin (), blend.tint.end (), out.tint);
		std::copy (input.normal.begin (), input.normal.end (), out.normal);
	}
	mesh.indexOffset = vertices.size () * sizeof (Vertex);
	mesh.indexCount = static_cast<uint32_t> (source.indices.size ());
	mesh.oceanIndexCount = source.oceanIndexCount;
	mesh.bytes = mesh.indexOffset + source.indices.size () * sizeof (uint32_t);
	R_CreateBuffer (
		&mesh.buffer, &mesh.memory, mesh.bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, nullptr, nullptr, "terrain LOD tile");
	std::vector<byte> storage (mesh.bytes);
	memcpy (storage.data (), vertices.data (), mesh.indexOffset);
	memcpy (storage.data () + mesh.indexOffset, source.indices.data (), source.indices.size () * sizeof (uint32_t));
	R_StagingUploadBuffer (mesh.buffer, mesh.bytes, storage.data ());
	allocatedBytes += mesh.bytes;
	return mesh;
}
void PublishStats (RenderStats stats)
{
	stats.gpuBytes = allocatedBytes;
	for (const auto &entry : retired)
		stats.retiredBytes += entry.mesh.bytes;
	std::lock_guard lock (statsMutex);
	renderStats = stats;
}
} // namespace

void R_OctreeInit ()
{
	static bool registered;
	if (!registered)
	{
		Cvar_RegisterVariable (&r_octree_lod);
		Cvar_RegisterVariable (&r_octree_lod_distance);
		Cmd_AddCommand ("octree_render_stats", R_OctreeStats);
		Cvar_RegisterVariable (&r_octree_distance);
		Cvar_RegisterVariable (&r_octree_mesh_mb);
		Cvar_RegisterVariable (&r_octree_upload_mb);
		Cvar_RegisterVariable (&r_octree_smooth);
		registered = true;
	}
}

void R_OctreePrepare ()
{
	R_OctreeInit ();

	auto prepared = std::make_shared<PreparedFrame> ();
	if (OW_Active () && OW_GetWorld ())
	{
		quiver::World &world = *OW_GetWorld ();
		world.SetSmoothTerrain (!std::isfinite (r_octree_smooth.value) || r_octree_smooth.value != 0);
		const quiver::WorldPosition camera = OW_GetRenderPosition ();
		const quiver::WorldPosition origin = OW_GetRenderOrigin ();
		const double				distance = std::isfinite (r_octree_distance.value) ? CLAMP (1024.0, r_octree_distance.value, 65536.0) : 4096.0;
		prepared->origin = origin;
		prepared->config = world.Config ();
		prepared->seed = world.Config ().seed;
		prepared->proceduralTerrain = world.Config ().generator != quiver::Generator::Empty;
		// Snapshot the sky's sun controls on the owning thread. Changing the sun
		// only changes push constants; cached geometry and normals stay valid.
		const double elevation = (std::isfinite (r_sun_angle.value) ? std::remainder (double (r_sun_angle.value), 360.0) : 45.0) * M_PI / 180.0;
		const double azimuth = (std::isfinite (r_sun_azimuth.value) ? std::remainder (double (r_sun_azimuth.value), 360.0) : 0.0) * M_PI / 180.0;
		prepared->sunlight = {
			float (std::cos (elevation) * std::sin (azimuth)), float (std::cos (elevation) * std::cos (azimuth)), float (std::sin (elevation)),
			std::isfinite (r_sun_intensity.value) ? CLAMP (0.0f, r_sun_intensity.value, 64.0f) : 1.0f};
		prepared->memoryBudget =
			static_cast<size_t> (std::isfinite (r_octree_mesh_mb.value) ? CLAMP (16.0f, r_octree_mesh_mb.value, 2048.0f) : 128.0f) * 1024 * 1024;
		prepared->uploadBudget =
			static_cast<size_t> (std::isfinite (r_octree_upload_mb.value) ? CLAMP (1.0f, r_octree_upload_mb.value, 128.0f) : 8.0f) * 1024 * 1024;
		std::set<quiver::TerrainTileKey, quiver::TerrainTileLess> detailedTiles;
		if (r_octree_lod.value && prepared->proceduralTerrain)
		{
			const double farDistance =
				std::isfinite (r_octree_lod_distance.value) ? CLAMP (4096.0, r_octree_lod_distance.value, quiver::MaxTerrainDistance) : 32768.0;
			const int detailRadius = std::min (8, int (std::ceil (distance / quiver::ChunkSize)) + 1);
			auto	  keys = quiver::SelectTerrainTiles (camera, farDistance, detailRadius);
			for (const auto &key : keys)
			{
				const double		  span = double (quiver::ChunkSize) * (uint64_t (1) << key.level);
				quiver::WorldPosition position{{key.x, key.y, 0}, {}};
				std::array<double, 3> translation, delta;
				if (!quiver::Delta (position, origin, translation, quiver::MaxTerrainDistance * 4) ||
					!quiver::Delta (position, camera, delta, quiver::MaxTerrainDistance * 4))
					continue;
				delta[0] += span * 0.5;
				delta[1] += span * 0.5;
				prepared->tiles.push_back ({key, {}, translation, delta[0] * delta[0] + delta[1] * delta[1]});
				if (!key.level)
					detailedTiles.insert (
						{quiver::CanonicalWorldKey (world.Config (), {key.x, key.y, 0}).x, quiver::CanonicalWorldKey (world.Config (), {key.x, key.y, 0}).y,
						 0});
			}
			std::sort (prepared->tiles.begin (), prepared->tiles.end (), [] (const auto &a, const auto &b) { return a.distanceSquared < b.distanceSquared; });
			keys.clear ();
			for (const auto &tile : prepared->tiles)
				keys.push_back (tile.key);
			if (!terrainCache || terrainSeed != world.Config ().seed || terrainSmooth != world.Config ().smoothTerrain ||
				terrainGenerator != world.Config ().generator || terrainPlanet.lock () != world.Config ().planet)
			{
				terrainCache = std::make_unique<quiver::TerrainLodCache> (world.Config ());
				terrainSeed = world.Config ().seed;
				terrainSmooth = world.Config ().smoothTerrain;
				terrainGenerator = world.Config ().generator;
				terrainPlanet = world.Config ().planet;
				++terrainGeneration;
			}
			prepared->terrainGeneration = terrainGeneration;
			terrainCache->Update (keys);
			prepared->cacheStats = terrainCache->Stats ();
			for (auto &tile : prepared->tiles)
				tile.mesh = terrainCache->Find (tile.key);
		}
		else
			terrainCache.reset ();

		for (const auto &key : world.ResidentKeys ())
		{
			if (!prepared->tiles.empty () && !detailedTiles.contains ({key.x, key.y, 0}))
				continue;
			quiver::WorldPosition position{key, {512.0f, 512.0f, 512.0f}};
			std::array<double, 3> delta;
			if (!quiver::WorldDelta (world.Config (), position, camera, delta, distance + quiver::ChunkSize))
				continue;
			double distanceSquared = 0;
			for (double d : delta)
				distanceSquared += d * d;
			if (distanceSquared > (distance + quiver::ChunkSize) * (distance + quiver::ChunkSize))
				continue;
			position.local = {};
			std::array<double, 3> translation;
			if (!quiver::WorldDelta (world.Config (), position, origin, translation))
				continue;
			prepared->chunks.push_back ({key, nullptr, translation, distanceSquared});
		}
		std::sort (
			prepared->chunks.begin (), prepared->chunks.end (), [] (const auto &a, const auto &b)
			{ return a.distanceSquared != b.distanceSquared ? a.distanceSquared < b.distanceSquared : KeyLess{}(a.key, b.key); });
		// Reserve CPU mesh capacity before requesting worker jobs. Requesting the
		// entire view repeatedly evicts meshes which the next frame immediately
		// rebuilds, especially after travel fills the resident chunk cache.
		const size_t unknownBytes =
			std::min (world.Config ().meshBytes, size_t (world.Config ().smoothTerrain && prepared->proceduralTerrain ? 16 : 1) * 1024 * 1024);
		size_t remainingBytes = world.Config ().meshBytes;
		size_t admitted = 0;
		for (size_t candidate = 0; candidate < prepared->chunks.size (); ++candidate)
		{
			auto		&chunk = prepared->chunks[candidate];
			const size_t bytes = world.MeshFootprint (chunk.key).value_or (unknownBytes);
			// A failed or individually oversized mesh cannot become admissible
			// by evicting other chunks. Leave its LOD fallback and keep looking.
			if (bytes > world.Config ().meshBytes)
				continue;
			if (bytes > remainingBytes)
				break;
			remainingBytes -= bytes;
			chunk.mesh = world.Mesh (chunk.key);
			if (admitted != candidate)
				prepared->chunks[admitted] = std::move (chunk);
			++admitted;
		}
		// Pending members retain their GPU meshes while rebuilding. Beyond this
		// stable nearest prefix, level-zero LOD tiles provide the fallback cover.
		prepared->chunks.resize (admitted);
	}
	else
		terrainCache.reset ();
	std::lock_guard lock (preparedMutex);
	preparedFrame = std::move (prepared);
}

void R_OctreeFrameReady ()
{
	frameSerial.fetch_add (1, std::memory_order_relaxed);
}

void R_OctreeDraw (cb_context_t *cbx)
{
	wireDraws.clear ();
	std::shared_ptr<const PreparedFrame> prepared;
	{
		std::lock_guard lock (preparedMutex);
		prepared = preparedFrame;
	}
	const uint64_t frame = frameSerial.load (std::memory_order_relaxed);
	CollectMeshes (frame);
	RenderStats												  stats;
	std::set<quiver::ChunkKey, KeyLess>						  wanted;
	std::set<quiver::TerrainTileKey, quiver::TerrainTileLess> wantedTiles;
	if (prepared)
	{
		stats.cache = prepared->cacheStats;
		for (const auto &chunk : prepared->chunks)
			wanted.insert (chunk.key);
		for (const auto &tile : prepared->tiles)
			wantedTiles.insert (tile.key);
	}
	for (auto it = meshes.begin (); it != meshes.end ();)
		if (!wanted.contains (it->first))
		{
			RetireMesh (it->second, frame);
			it = meshes.erase (it);
		}
		else
			++it;
	// Retain an old coarse tile until every overlapping replacement is uploaded.
	// The old frontier stays visible while bounded jobs/uploads catch up.
	auto collectTerrain = [&]
	{
		for (auto it = terrainMeshes.begin (); it != terrainMeshes.end ();)
		{
			if (wantedTiles.contains (it->first))
			{
				++it;
				continue;
			}
			bool redundant = false;
			for (const auto &[other, unused] : terrainMeshes)
				if (other.level > it->first.level && !wantedTiles.contains (other) && TileContains (other, it->first))
				{
					redundant = true;
					break;
				}
			bool overlaps = false, complete = true;
			for (const auto &key : wantedTiles)
				if (TilesOverlap (key, it->first))
				{
					overlaps = true;
					complete &= terrainMeshes.contains (key);
				}
			if (redundant || !overlaps || complete)
			{
				RetireMesh (it->second.mesh, frame);
				it = terrainMeshes.erase (it);
			}
			else
				++it;
		}
	};
	collectTerrain ();
	const size_t horizonReserve = prepared && !prepared->tiles.empty () ? std::min (prepared->memoryBudget / 4, size_t (8 * 1024 * 1024)) : 0;
	const size_t nearBudget = prepared ? prepared->memoryBudget - horizonReserve : 0;
	std::set<quiver::ChunkKey, KeyLess> admittedNear;
	size_t								desiredNearBytes = 0;
	if (prepared)
		for (const auto &chunk : prepared->chunks)
		{
			auto current = meshes.find (chunk.key);
			if (!chunk.mesh && current == meshes.end ())
				continue;
			const size_t bytes = chunk.mesh && (current == meshes.end () || current->second.serial != chunk.mesh->serial) ? MeasureMesh (*chunk.mesh).Bytes ()
																														  : current->second.bytes;
			if (desiredNearBytes + bytes > nearBudget)
				break;
			desiredNearBytes += bytes;
			admittedNear.insert (chunk.key);
		}
	// A deterministic nearest prefix prevents smaller, farther chunks from
	// repeatedly occupying memory while a nearer replacement waits for fences.
	for (auto it = meshes.begin (); it != meshes.end ();)
		if (!admittedNear.contains (it->first))
		{
			RetireMesh (it->second, frame);
			it = meshes.erase (it);
		}
		else
			++it;
	const size_t farBudget = prepared ? prepared->memoryBudget - desiredNearBytes : 0;
	if (prepared)
	{
		size_t retiring = 0, nearRetiring = 0;
		for (const auto &entry : retired)
		{
			retiring += entry.mesh.bytes;
			if (!entry.mesh.terrain)
				nearRetiring += entry.mesh.bytes;
		}
		// Keep a small horizon share even under the smallest budget. Without
		// this admission limit, unsatisfied near demand evicts the same distant
		// tiles every frame and a stationary view never settles.
		for (auto far = prepared->chunks.rbegin (); far != prepared->chunks.rend () && nearAllocatedBytes - nearRetiring > nearBudget; ++far)
		{
			auto victim = meshes.find (far->key);
			if (victim == meshes.end ())
				continue;
			retiring += victim->second.bytes;
			nearRetiring += victim->second.bytes;
			RetireMesh (victim->second, frame);
			meshes.erase (victim);
		}
		// Also honor a total budget lowered while every mesh is already cached.
		auto overFarBudget = [&] { return allocatedBytes - nearAllocatedBytes - (retiring - nearRetiring) > farBudget; };
		for (auto far = prepared->tiles.rbegin (); far != prepared->tiles.rend () && (overFarBudget () || allocatedBytes - retiring > prepared->memoryBudget);
			 ++far)
		{
			auto victim = terrainMeshes.find (far->key);
			if (victim == terrainMeshes.end ())
				continue;
			retiring += victim->second.mesh.bytes;
			RetireMesh (victim->second.mesh, frame);
			terrainMeshes.erase (victim);
		}
		for (auto it = terrainMeshes.begin (); it != terrainMeshes.end () && (overFarBudget () || allocatedBytes - retiring > prepared->memoryBudget);)
		{
			retiring += it->second.mesh.bytes;
			RetireMesh (it->second.mesh, frame);
			it = terrainMeshes.erase (it);
		}
	}
	if (!prepared || (prepared->chunks.empty () && prepared->tiles.empty ()) || !r_drawworld_cheatsafe)
	{
		PublishStats (stats);
		return;
	}

	CreateMaterialTexture ();
	size_t uploaded = 0;
	for (const auto &chunk : prepared->chunks)
	{
		if (!chunk.mesh || !admittedNear.contains (chunk.key))
			continue;
		auto found = meshes.find (chunk.key);
		if (found != meshes.end () && found->second.serial == chunk.mesh->serial)
			continue;
		const size_t bytes = MeasureMesh (*chunk.mesh).Bytes ();
		if (bytes > nearBudget || (uploaded && uploaded + bytes > prepared->uploadBudget))
			continue;
		if (nearAllocatedBytes + bytes > nearBudget)
		{
			size_t reclaiming = 0;
			for (const auto &entry : retired)
				if (!entry.mesh.terrain)
					reclaiming += entry.mesh.bytes;
			if (nearAllocatedBytes + bytes > nearBudget + reclaiming)
			{
				if (found != meshes.end ())
				{
					RetireMesh (found->second, frame);
					meshes.erase (found);
				}
				else
					for (auto far = prepared->chunks.rbegin (); far != prepared->chunks.rend (); ++far)
					{
						if (far->distanceSquared <= chunk.distanceSquared)
							break;
						auto victim = meshes.find (far->key);
						if (victim != meshes.end ())
						{
							RetireMesh (victim->second, frame);
							meshes.erase (victim);
							break;
						}
					}
			}
			continue;
		}
		if (allocatedBytes + bytes > prepared->memoryBudget)
		{
			// Near detail wins over the horizon. Retired memory remains charged
			// until its fences complete; no replacement can exceed the budget.
			if (!terrainMeshes.empty ())
			{
				size_t released = 0;
				for (auto it = terrainMeshes.begin (); it != terrainMeshes.end () && allocatedBytes + bytes > prepared->memoryBudget + released;)
				{
					released += it->second.mesh.bytes;
					RetireMesh (it->second.mesh, frame);
					it = terrainMeshes.erase (it);
				}
			}
			else if (found != meshes.end ())
			{
				RetireMesh (found->second, frame);
				meshes.erase (found);
			}
			else
				for (auto far = prepared->chunks.rbegin (); far != prepared->chunks.rend (); ++far)
				{
					if (far->distanceSquared <= chunk.distanceSquared)
						break;
					auto victim = meshes.find (far->key);
					if (victim != meshes.end ())
					{
						RetireMesh (victim->second, frame);
						meshes.erase (victim);
						break;
					}
				}
			continue;
		}
		GpuMesh replacement = UploadMesh (*chunk.mesh, prepared->seed, prepared->proceduralTerrain);
		if (found != meshes.end ())
			RetireMesh (found->second, frame);
		meshes[chunk.key] = replacement;
		uploaded += bytes;
	}
	for (const auto &tile : prepared->tiles)
	{
		auto current = terrainMeshes.find (tile.key);
		if (!tile.mesh || (current != terrainMeshes.end () && current->second.generation == prepared->terrainGeneration))
			continue;
		const size_t bytes = tile.mesh->vertices.size () * sizeof (Vertex) + tile.mesh->indices.size () * sizeof (uint32_t);
		if (uploaded + bytes > prepared->uploadBudget)
			continue;
		// Reserve the cost of admitted nearby meshes even while their old
		// allocations retire; tiny far uploads cannot steal each freed slot.
		if (allocatedBytes - nearAllocatedBytes + bytes > farBudget || allocatedBytes + bytes > prepared->memoryBudget)
		{
			// A retained transition parent cannot wait forever for replacements
			// that do not fit beside it. Release obsolete coverage first, then
			// farther selected tiles. Under pressure fences can leave a temporary
			// far gap; they must never force memory beyond the shared limit.
			size_t reclaiming = 0, farReclaiming = 0;
			for (const auto &entry : retired)
			{
				reclaiming += entry.mesh.bytes;
				if (entry.mesh.terrain)
					farReclaiming += entry.mesh.bytes;
			}
			if (allocatedBytes + bytes > prepared->memoryBudget + reclaiming || allocatedBytes - nearAllocatedBytes + bytes > farBudget + farReclaiming)
			{
				auto victim = terrainMeshes.end ();
				for (auto it = terrainMeshes.begin (); it != terrainMeshes.end (); ++it)
					if (!wantedTiles.contains (it->first))
					{
						victim = it;
						break;
					}
				if (victim == terrainMeshes.end ())
					for (auto far = prepared->tiles.rbegin (); far != prepared->tiles.rend (); ++far)
					{
						if (far->distanceSquared <= tile.distanceSquared)
							break;
						auto found = terrainMeshes.find (far->key);
						if (found != terrainMeshes.end ())
						{
							victim = found;
							break;
						}
					}
				if (victim == terrainMeshes.end ())
					victim = current;
				if (victim != terrainMeshes.end ())
				{
					RetireMesh (victim->second.mesh, frame);
					terrainMeshes.erase (victim);
				}
			}
			continue;
		}
		GpuMesh mesh = UploadTerrain (*tile.mesh);
		if (!mesh.indexCount)
			continue;
		if (current != terrainMeshes.end ())
			RetireMesh (current->second.mesh, frame);
		terrainMeshes[tile.key] =
			GpuTerrain{mesh, tile.mesh->boundsMin, tile.mesh->boundsMax, tile.mesh->surfaceMin, tile.mesh->surfaceMax, prepared->terrainGeneration};
		uploaded += bytes;
	}
	collectTerrain ();
	std::vector<quiver::TerrainTileKey> oldFrontier;
	for (const auto &[key, mesh] : terrainMeshes)
		if (!wantedTiles.contains (key))
			oldFrontier.push_back (key);

	R_BeginDebugUtilsLabel (cbx, "Octree chunks and terrain LOD");
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.terrain_pipeline);
	std::array<VkDescriptorSet, 4> sets;
	for (size_t i = 0; i < sets.size (); ++i)
		sets[i] = materialTextures[i]->descriptor_set;
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.terrain_pipeline_layout.handle, 0, sets.size (), sets.data (), 0, nullptr);
	Fog_EnableGFog (cbx);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 20 * sizeof (float), sizeof (prepared->sunlight), prepared->sunlight.data ());
	const bool showTris = R_ShowTrisEnabled ();
	uint32_t brushPasses = 0;
	auto draw = [&] (
					const GpuMesh &mesh, quiver::ChunkKey key, const std::array<double, 3> &position, const std::array<float, 3> &boundsMin,
					const std::array<float, 3> &boundsMax, bool ground = true, unsigned transitions = 0) -> uint32_t
	{
		if (!mesh.indexCount)
			return 0;
		vec3_t mins, maxs;
		for (int axis = 0; axis < 3; ++axis)
		{
			mins[axis] = float (position[axis]) + boundsMin[axis];
			maxs[axis] = float (position[axis]) + boundsMax[axis];
		}
		if (R_CullBoxWithFarClip (mins, maxs))
			return 0;
		std::array<float, 16> matrix;
		float translation[16];
		memcpy (matrix.data (), vulkan_globals.view_projection_matrix, sizeof (matrix));
		TranslationMatrix (translation, float (position[0]), float (position[1]), float (position[2]));
		MatrixMultiply (matrix.data (), translation);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (matrix), matrix.data ());
		const std::array<float, 4> curvature = {
			float (position[0]) - r_refdef.vieworg[0], float (position[1]) - r_refdef.vieworg[1], r_planet_bend_active ? 1.0f : 0.0f,
			float (r_world_debug_lighting)};
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 112, sizeof (curvature), curvature.data ());
		// Unsigned modulo remains defined for negative and extreme coordinates.
		// Only the texture/noise period enters floats, never the absolute world.
		std::array<float, 4> phase = {
			quiver::TerrainMaterialChunkPhase (key.x), quiver::TerrainMaterialChunkPhase (key.y), quiver::TerrainMaterialChunkPhase (key.z), 0};
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 24 * sizeof (float), sizeof (phase), phase.data ());
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers (cbx->cb, 0, 1, &mesh.buffer, &offset);
		vkCmdBindIndexBuffer (cbx->cb, mesh.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
		uint32_t drawnIndices = 0;
		auto drawRange = [&] (uint32_t first, uint32_t count)
		{
			if (!count)
				return;
			vkCmdDrawIndexed (cbx->cb, count, 1, first, 0, 0);
			++brushPasses;
			drawnIndices += count;
			if (showTris)
				wireDraws.push_back ({mesh.buffer, mesh.indexOffset, first, count, matrix, curvature});
		};
		if (ground)
			drawRange (0, mesh.indexCount - mesh.oceanIndexCount);
		uint32_t firstTransition = mesh.indexCount;
		for (int side = 0; side < 4; ++side)
		{
			if ((transitions & (1u << side)) && mesh.transitionCounts[side])
				drawRange (firstTransition, mesh.transitionCounts[side]);
			firstTransition += mesh.transitionCounts[side];
		}
		if (mesh.oceanIndexCount)
		{
			phase[3] = 1;
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 24 * sizeof (float), sizeof (phase), phase.data ());
			drawRange (mesh.indexCount - mesh.oceanIndexCount, mesh.oceanIndexCount);
		}
		return drawnIndices / 3;
	};
	auto columnDetailed = [&] (quiver::TerrainTileKey key, const GpuTerrain &terrain)
	{
		if (key.level)
			return false;
		const auto	  canonical = quiver::CanonicalWorldKey (prepared->config, {key.x, key.y, 0});
		const int64_t firstZ = int64_t (std::floor (terrain.surfaceMin / quiver::ChunkSize));
		const int64_t lastZ = int64_t (std::floor (terrain.surfaceMax / quiver::ChunkSize));
		if (lastZ - firstZ >= 256)
			return false;
		for (int64_t z = firstZ; z <= lastZ; ++z)
			if (!wanted.contains ({canonical.x, canonical.y, z}) || !admittedNear.contains ({canonical.x, canonical.y, z}) ||
				!meshes.contains ({canonical.x, canonical.y, z}))
				return false;
		return true;
	};
	std::set<quiver::TerrainTileKey, quiver::TerrainTileLess> detailedColumns;
	for (const auto &[key, terrain] : terrainMeshes)
	{
		if (!columnDetailed (key, terrain))
			continue;
		bool covered = false;
		for (const auto &old : oldFrontier)
			covered |= old.level && TileContains (old, key);
		if (!covered)
			detailedColumns.insert (key);
	}
	for (const auto &chunk : prepared->chunks)
	{
		auto found = meshes.find (chunk.key);
		if (found == meshes.end ())
			continue;
		// A transitioning coarse tile is a temporary fallback only at the edge
		// of detail. Avoid overlapping it until its new frontier is complete.
		const quiver::TerrainTileKey column{
			prepared->origin.chunk.x + int64_t (std::llround ((chunk.translation[0] + prepared->origin.local[0]) / quiver::ChunkSize)),
			prepared->origin.chunk.y + int64_t (std::llround ((chunk.translation[1] + prepared->origin.local[1]) / quiver::ChunkSize)), 0};
		bool covered = false;
		for (const auto &key : oldFrontier)
			covered |= key.level && TileContains (key, column);
		// Publish complete vertical surface columns together. Drawing only some
		// chunks over the fallback causes holes or coplanar flicker on mountains.
		if (auto fallback = terrainMeshes.find (column); fallback != terrainMeshes.end ())
			covered |= !columnDetailed (column, fallback->second);
		if (covered)
			continue;
		unsigned transitions = 0;
		if (!prepared->tiles.empty ())
			for (int side = 0; side < 4; ++side)
			{
				auto neighbor = column;
				(side / 2 ? neighbor.y : neighbor.x) += (side & 1) ? 1 : -1;
				if (!detailedColumns.contains (neighbor))
					transitions |= 1u << side;
			}
		if (const auto triangles = draw (
				found->second, chunk.key, chunk.translation, {-16, -16, -quiver::ChunkSize},
				{quiver::ChunkSize + 16, quiver::ChunkSize + 16, 2 * quiver::ChunkSize}, true, transitions))
		{
			++stats.nearDraws;
			stats.nearTriangles += triangles;
		}
	}
	for (const auto &[key, terrain] : terrainMeshes)
	{
		const bool detailed = detailedColumns.contains (key);
		if (detailed && !terrain.mesh.oceanIndexCount)
			continue;
		bool covered = false;
		for (const auto &old : oldFrontier)
			if (!(old == key))
				covered |= wantedTiles.contains (key) ? TilesOverlap (key, old) : TileContains (old, key);
		if (covered)
			continue;
		std::array<double, 3> position;
		if (!quiver::Delta ({{key.x, key.y, 0}, {}}, prepared->origin, position, quiver::MaxTerrainDistance * 4))
			continue;
		if (const auto triangles = draw (terrain.mesh, {key.x, key.y, 0}, position, terrain.mins, terrain.maxs, !detailed))
		{
			++stats.farDraws;
			stats.farTriangles += triangles;
		}
	}
	stats.uploadedBytes = uploaded;
	// Streamed polygons are triangles; a brush pass is one submitted draw,
	// as in R_FlushBatch. The diagnostic overlay does not count twice.
	Atomic_AddUInt32 (&rs_brushpolys, uint32_t (stats.nearTriangles + stats.farTriangles));
	Atomic_AddUInt32 (&rs_brushpasses, brushPasses);
	PublishStats (stats);
	// Changing push-constant ranges clears the engine's cached constants. Leave
	// the ordinary world pipeline with its matrix/fog ready for later draws.
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipelines[0]);
	Fog_EnableGFog (cbx);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (vulkan_globals.view_projection_matrix), vulkan_globals.view_projection_matrix);
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	R_EndDebugUtilsLabel (cbx);
}

void R_OctreeDrawShowTris (cb_context_t *cbx)
{
	if (wireDraws.empty ())
		return;
	const bool depthTest = r_showtris.value != 1;
	R_BeginDebugUtilsLabel (cbx, "Octree show tris");
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.terrain_showtris_pipelines[depthTest]);
	const VkDeviceSize offset = 0;
	for (const auto &draw : wireDraws)
	{
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (draw.matrix), draw.matrix.data ());
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 112, sizeof (draw.curvature), draw.curvature.data ());
		vkCmdBindVertexBuffers (cbx->cb, 0, 1, &draw.buffer, &offset);
		vkCmdBindIndexBuffer (cbx->cb, draw.buffer, draw.indexOffset, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed (cbx->cb, draw.count, 1, draw.first, 0, 0);
	}
	// Subsequent stock debug draws expect the world view matrix and fan indices.
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS,
		depthTest ? vulkan_globals.showtris_depth_test_pipeline : vulkan_globals.showtris_pipeline);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (vulkan_globals.view_projection_matrix), vulkan_globals.view_projection_matrix);
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	R_EndDebugUtilsLabel (cbx);
}
void R_OctreeStats ()
{
	RenderStats stats;
	{
		std::lock_guard lock (statsMutex);
		stats = renderStats;
	}
	Con_Printf (
		"Octree render: near %zu draws %zu triangles, far %zu draws %zu triangles\n", stats.nearDraws, stats.nearTriangles, stats.farDraws, stats.farTriangles);
	Con_Printf (
		"Octree GPU: %.3f MiB live+retired, %.3f MiB retired, uploaded %.3f KiB this frame\n", stats.gpuBytes / 1048576.0, stats.retiredBytes / 1048576.0,
		stats.uploadedBytes / 1024.0);
	Con_Printf (
		"Terrain LOD: %zu selected, %zu cached, %zu pending, %.3f MiB CPU, %zu failed, %.3f ms total builds\n", stats.cache.selected, stats.cache.cached,
		stats.cache.pending, stats.cache.bytes / 1048576.0, stats.cache.failed, stats.cache.buildMilliseconds);
}

void R_OctreeReset ()
{
	wireDraws.clear ();
	terrainCache.reset ();
	terrainPlanet.reset ();
	for (auto &[key, terrain] : terrainMeshes)
		FreeMesh (terrain.mesh);
	terrainMeshes.clear ();
	{
		std::lock_guard lock (statsMutex);
		renderStats = {};
	}
	for (auto &[key, mesh] : meshes)
		FreeMesh (mesh);
	meshes.clear ();
	for (auto &entry : retired)
		FreeMesh (entry.mesh);
	retired.clear ();
	for (auto &texture : materialTextures)
	{
		if (texture)
			TexMgr_FreeTexture (texture);
		texture = nullptr;
	}
	std::lock_guard lock (preparedMutex);
	preparedFrame.reset ();
}
