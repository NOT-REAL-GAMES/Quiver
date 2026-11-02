/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_mesher.h"
#include "terrain_density.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace quiver
{
namespace
{
constexpr int	 SampleSide = VoxelsPerChunk + 4;
constexpr size_t MaxVertices = 262144;
constexpr size_t MaxIndices = MaxVertices * 6;
using Vec3 = std::array<float, 3>;
using PrecisePosition = std::array<double, 3>;
using Point = std::array<int, 3>;

Vec3 Subtract (const Vec3 &a, const Vec3 &b)
{
	return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 Cross (const Vec3 &a, const Vec3 &b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float Dot (const Vec3 &a, const Vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Unit (Vec3 normal)
{
	const float squared = Dot (normal, normal);
	if (squared < 1e-16f)
		return {0, 0, 1};
	const float inverse = 1 / std::sqrt (squared);
	for (float &value : normal)
		value *= inverse;
	return normal;
}

using Sample = TerrainDensitySample;
using Column = TerrainDensityColumn;

class Builder
{
  public:
	Builder (
		const WorldConfig &worldConfig, const std::array<std::shared_ptr<const Chunk>, 27> &snapshots, const ChunkMesh &source, Point first = {},
		Point count = {VoxelsPerChunk, VoxelsPerChunk, VoxelsPerChunk}, bool region = false)
		: config (worldConfig), chunks (snapshots), key (source.key), regionSubset (region)
	{
		if (!chunks[13] || chunks[13]->key != key)
			throw std::runtime_error ("invalid smooth terrain center snapshot");
		size_t cubes = 1;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (first[axis] < 0 || count[axis] <= 0 || first[axis] > VoxelsPerChunk - count[axis])
				throw std::runtime_error ("invalid smooth terrain region bounds");
			cubeMinimum[axis] = first[axis] - 1;
			cubeMaximum[axis] = first[axis] + count[axis] - 1;
			cubeSides[axis] = count[axis] + 1;
			sampleMinimum[axis] = first[axis] - 2;
			sampleSides[axis] = count[axis] + 4;
			clipMinimum[axis] = first[axis] * VoxelSize;
			clipMaximum[axis] = (first[axis] + count[axis]) * VoxelSize;
			cubes *= cubeSides[axis];
		}
		candidates.resize ((cubes + 63) / 64);
		columns.resize (sampleSides[0] * sampleSides[1]);
		samples.resize (4 * columns.size ());
		layers.fill (std::numeric_limits<int>::min ());
		if (region)
		{
			// A collision tile has no exposed-quad prepass. Its bounded cube
			// range includes the half-cell halo clipped at exact tile planes.
			Mark (cubeMinimum, cubeMaximum);
			return;
		}
		for (const auto &quad : source.quads)
		{
			const int axis = quad.axis, u = (axis + 1) % 3, v = (axis + 2) % 3;
			const int plane = int (std::lround (quad.origin[axis] / VoxelSize));
			Point	  minimum{}, maximum{};
			minimum[axis] = plane - 2;
			maximum[axis] = plane;
			minimum[u] = int (quad.origin[u] / VoxelSize) - 1;
			minimum[v] = int (quad.origin[v] / VoxelSize) - 1;
			maximum[u] = int ((quad.origin[u] + quad.size) / VoxelSize);
			maximum[v] = int ((quad.origin[v] + quad.size) / VoxelSize);
			Mark (minimum, maximum);
		}
		// A neighboring edit can extend half a cell into an otherwise empty
		// chunk, which has no exposed quad of its own to seed that cube.
		for (int axis = 0; axis < 3; ++axis)
			for (int boundary : {-1, VoxelsPerChunk - 1})
			{
				Point minimum{-1, -1, -1}, maximum{VoxelsPerChunk - 1, VoxelsPerChunk - 1, VoxelsPerChunk - 1};
				minimum[axis] = maximum[axis] = boundary;
				Mark (minimum, maximum);
			}
	}

	void Run (ChunkMesh &mesh)
	{
		for (int z = cubeMinimum[2]; z <= cubeMaximum[2]; ++z)
			for (int y = cubeMinimum[1]; y <= cubeMaximum[1]; ++y)
				for (int x = cubeMinimum[0]; x <= cubeMaximum[0]; ++x)
				{
					const size_t bit = CubeIndex ({x, y, z});
					if (candidates[bit / 64] & (uint64_t (1) << (bit % 64)))
						Cube ({x, y, z});
				}
		// Clipping can leave unused intersections outside the chunk. Compact in
		// place so renderer bounds, GPU bytes and all published vertices agree.
		std::vector<uint32_t> remap (vertices.size (), UINT32_MAX);
		for (uint32_t index : indices)
			remap[index] = 0;
		size_t count = 0;
		for (size_t i = 0; i < vertices.size (); ++i)
			if (remap[i] != UINT32_MAX)
			{
				remap[i] = uint32_t (count);
				vertices[count++] = vertices[i];
			}
		for (uint32_t &index : indices)
			index = remap[index];
		vertices.resize (count);
		vertices.shrink_to_fit ();
		indices.shrink_to_fit ();
		mesh.vertices = std::move (vertices);
		mesh.indices = std::move (indices);
		mesh.smooth = true;
	}

  private:
	const WorldConfig								   &config;
	const std::array<std::shared_ptr<const Chunk>, 27> &chunks;
	ChunkKey											key;
	bool												regionSubset;
	Point												cubeMinimum{}, cubeMaximum{}, cubeSides{}, sampleMinimum{}, sampleSides{}, clipMinimum{}, clipMaximum{};
	std::vector<uint64_t>								candidates;
	std::vector<Column>									columns;
	std::vector<Sample>									samples;
	std::array<int, 4>									layers;
	std::unordered_map<uint64_t, uint32_t>				edges;
	std::vector<MeshVertex>								vertices;
	std::vector<PrecisePosition>						positions;
	std::vector<uint32_t>								indices;

	size_t CubeIndex (Point p) const
	{
		return (size_t (p[2] - cubeMinimum[2]) * cubeSides[1] + p[1] - cubeMinimum[1]) * cubeSides[0] + p[0] - cubeMinimum[0];
	}
	size_t ColumnIndex (int x, int y) const
	{
		return size_t (y - sampleMinimum[1]) * sampleSides[0] + x - sampleMinimum[0];
	}

	void Mark (Point minimum, Point maximum)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			minimum[axis] = std::max (cubeMinimum[axis], minimum[axis]);
			maximum[axis] = std::min (cubeMaximum[axis], maximum[axis]);
		}
		for (int z = minimum[2]; z <= maximum[2]; ++z)
			for (int y = minimum[1]; y <= maximum[1]; ++y)
				for (int x = minimum[0]; x <= maximum[0]; ++x)
				{
					const size_t bit = CubeIndex ({x, y, z});
					candidates[bit / 64] |= uint64_t (1) << (bit % 64);
				}
	}

	Column &GetColumn (int x, int y)
	{
		Column &column = columns[ColumnIndex (x, y)];
		if (!column.valid)
			column = TerrainSampleColumn (config, key, x, y);
		return column;
	}

	Sample &Get (Point point)
	{
		const int layer = (point[2] + 4) % 4;
		Sample	 *slab = samples.data () + layer * columns.size ();
		if (layers[layer] != point[2])
		{
			std::fill_n (slab, columns.size (), Sample{});
			layers[layer] = point[2];
		}
		Sample &sample = slab[ColumnIndex (point[0], point[1])];
		if (sample.valid)
			return sample;
		sample = TerrainSampleDensity (config, key, point, chunks, GetColumn (point[0], point[1]));
		return sample;
	}

	Vec3 Normal (Point point, bool editedEdge)
	{
		bool modified = editedEdge || Get (point).modified;
		Vec3 normal{};
		for (int axis = 0; axis < 3; ++axis)
		{
			Point low = point, high = point;
			--low[axis];
			++high[axis];
			const Sample lower = Get (low), upper = Get (high);
			modified |= lower.modified || upper.modified;
			normal[axis] = lower.density - upper.density;
		}
		if (config.generator == Generator::Terrain && !modified)
		{
			const Column &column = GetColumn (point[0], point[1]);
			return TerrainSurfaceNormal (config.seed, column.x, column.y, column.localX, column.localY);
		}
		// A lone occupied/empty sample has a zero centered gradient. Keep it
		// zero so the crossing edge inherits its other endpoint's direction.
		return Dot (normal, normal) < 1e-16f ? Vec3{} : Unit (normal);
	}

	uint32_t AddVertex (MeshVertex vertex, PrecisePosition position)
	{
		if (vertices.size () >= MaxVertices)
			throw std::runtime_error ("smooth terrain vertex limit exceeded");
		if (vertices.size () == vertices.capacity ())
		{
			const size_t capacity = std::min (MaxVertices, std::max (size_t (1024), vertices.capacity () * 2));
			if (capacity * sizeof (MeshVertex) + indices.capacity () * sizeof (uint32_t) > config.meshBytes)
				throw std::runtime_error ("smooth terrain exceeds CPU mesh budget");
			vertices.reserve (capacity);
		}
		// Boundary interpolation can leave cancellation noise of ~1e-13.
		// Canonicalize only that numerical residue, keeping adjacent chunks'
		// exact clip planes equal before conversion to GPU float positions.
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::abs (position[axis] - clipMinimum[axis]) < 1e-9)
				position[axis] = clipMinimum[axis];
			else if (std::abs (position[axis] - clipMaximum[axis]) < 1e-9)
				position[axis] = clipMaximum[axis];
			vertex.position[axis] = float (position[axis]);
		}
		vertices.push_back (vertex);
		positions.push_back (position);
		return uint32_t (vertices.size () - 1);
	}

	void AddTriangle (uint32_t a, uint32_t b, uint32_t c)
	{
		if (a == b || b == c || c == a)
			return;
		// A surface exactly on a tile's positive plane belongs to its
		// neighboring tile. Otherwise a horizontal floor on a Z boundary
		// would be emitted twice as coincident complete triangles.
		if (regionSubset)
			for (int axis = 0; axis < 3; ++axis)
				if (positions[a][axis] == clipMaximum[axis] && positions[b][axis] == clipMaximum[axis] && positions[c][axis] == clipMaximum[axis])
					return;
		const Vec3 cross = Cross (Subtract (vertices[b].position, vertices[a].position), Subtract (vertices[c].position, vertices[a].position));
		if (Dot (cross, cross) < 1e-12f)
			return;
		if (indices.size () + 3 > MaxIndices)
			throw std::runtime_error ("smooth terrain triangle limit exceeded");
		if (indices.size () + 3 > indices.capacity ())
		{
			const size_t capacity = std::min (MaxIndices, std::max (size_t (3072), indices.capacity () * 2));
			if (vertices.capacity () * sizeof (MeshVertex) + capacity * sizeof (uint32_t) > config.meshBytes)
				throw std::runtime_error ("smooth terrain exceeds CPU mesh budget");
			indices.reserve (capacity);
		}
		indices.insert (indices.end (), {a, b, c});
	}

	uint32_t Intersection (Point a, Point b)
	{
		auto sampleIndex = [] (Point point) -> uint32_t
		{ return uint32_t ((point[2] + 2) * SampleSide * SampleSide + (point[1] + 2) * SampleSide + point[0] + 2); };
		uint32_t first = sampleIndex (a), second = sampleIndex (b);
		if (first > second)
		{
			std::swap (first, second);
			std::swap (a, b);
		}
		const uint64_t edge = (uint64_t (first) << 22) | second;
		if (auto found = edges.find (edge); found != edges.end ())
			return found->second;
		const Sample	sa = Get (a), sb = Get (b);
		const double	fraction = double (sa.density) / (double (sa.density) - sb.density);
		const Vec3		na = Normal (a, sa.modified || sb.modified), nb = Normal (b, sa.modified || sb.modified);
		MeshVertex		vertex;
		PrecisePosition position;
		for (int axis = 0; axis < 3; ++axis)
		{
			position[axis] = double (a[axis] * VoxelSize + VoxelSize / 2) + fraction * double ((b[axis] - a[axis]) * VoxelSize);
			vertex.position[axis] = float (position[axis]);
			vertex.normal[axis] = na[axis] + fraction * (nb[axis] - na[axis]);
		}
		if (Dot (vertex.normal, vertex.normal) < 1e-16f)
			for (int axis = 0; axis < 3; ++axis)
				vertex.normal[axis] = float (b[axis] - a[axis]) * (sa.density > 0 ? 1 : -1);
		vertex.normal = Unit (vertex.normal);
		vertex.blend = TerrainClassifyMaterial (sa.density > 0 ? sa.material : sb.material, config.generator != Generator::Empty);
		const uint32_t index = AddVertex (vertex, position);
		edges.emplace (edge, index);
		return index;
	}

	uint32_t ClipIntersection (uint32_t a, uint32_t b, int plane)
	{
		if (a > b)
			std::swap (a, b);
		const uint64_t edge = (uint64_t (1) << 63) | (uint64_t (plane) << 40) | (uint64_t (a) << 20) | b;
		if (auto found = edges.find (edge); found != edges.end ())
			return found->second;
		const int			  axis = plane / 2;
		const double		  boundary = plane & 1 ? double (clipMaximum[axis]) : double (clipMinimum[axis]);
		const MeshVertex	  va = vertices[a], vb = vertices[b];
		const PrecisePosition pa = positions[a], pb = positions[b];
		const double		  fraction = (boundary - pa[axis]) / (pb[axis] - pa[axis]);
		if (fraction == 0)
			return a;
		if (fraction == 1)
			return b;
		MeshVertex		vertex;
		PrecisePosition position;
		for (int component = 0; component < 3; ++component)
		{
			position[component] = pa[component] + fraction * (pb[component] - pa[component]);
			vertex.position[component] = float (position[component]);
			vertex.normal[component] = va.normal[component] + fraction * (vb.normal[component] - va.normal[component]);
		}
		position[axis] = boundary;
		vertex.position[axis] = float (boundary);
		vertex.normal = Unit (vertex.normal);
		vertex.blend = TerrainInterpolateMaterials (va.blend, vb.blend, fraction);
		const uint32_t index = AddVertex (vertex, position);
		edges.emplace (edge, index);
		return index;
	}

	void Triangle (uint32_t a, uint32_t b, uint32_t c, const Vec3 &outward, bool boundary)
	{
		// Keep shared crossing vertices: interpolated weights blend materials
		// across triangle edges. Chunk clipping interpolates the same weights.
		const Vec3 cross = Cross (Subtract (vertices[b].position, vertices[a].position), Subtract (vertices[c].position, vertices[a].position));
		if (Dot (cross, outward) > 0)
			std::swap (b, c);
		if (!boundary)
		{
			AddTriangle (a, b, c);
			return;
		}
		std::array<uint32_t, 12> polygon{a, b, c};
		size_t					 count = 3;
		for (int plane = 0; plane < 6 && count; ++plane)
		{
			const int axis = plane / 2;
			auto	  inside = [&] (uint32_t index)
			{ return plane & 1 ? positions[index][axis] <= clipMaximum[axis] : positions[index][axis] >= clipMinimum[axis]; };
			std::array<uint32_t, 12> clipped{};
			size_t					 nextCount = 0;
			uint32_t				 previous = polygon[count - 1];
			bool					 previousInside = inside (previous);
			for (size_t i = 0; i < count; ++i)
			{
				const uint32_t current = polygon[i];
				const bool	   currentInside = inside (current);
				if (currentInside != previousInside)
					clipped[nextCount++] = ClipIntersection (previous, current, plane);
				if (currentInside)
					clipped[nextCount++] = current;
				previous = current;
				previousInside = currentInside;
			}
			polygon = clipped;
			count = nextCount;
		}
		for (size_t i = 1; i + 1 < count; ++i)
			AddTriangle (polygon[0], polygon[i], polygon[i + 1]);
	}

	void Cube (Point origin)
	{
		std::array<Point, 8> points;
		std::array<bool, 8>	 solid;
		int					 occupied = 0;
		for (int i = 0; i < 8; ++i)
		{
			points[i] = {origin[0] + (i & 1), origin[1] + ((i >> 1) & 1), origin[2] + ((i >> 2) & 1)};
			occupied += solid[i] = Get (points[i]).density > 0;
		}
		if (!occupied || occupied == 8)
			return;
		const bool boundary = origin[0] == cubeMinimum[0] || origin[1] == cubeMinimum[1] || origin[2] == cubeMinimum[2] || origin[0] == cubeMaximum[0] ||
							  origin[1] == cubeMaximum[1] || origin[2] == cubeMaximum[2];
		// Identical body diagonal and face diagonals in every cube, including
		// both copies of a clipped chunk-boundary cube, prevent tetra seams.
		for (const auto &tetrahedron : TerrainTetrahedra)
		{
			int	 in[4], out[4], inCount = 0, outCount = 0;
			Vec3 outward{};
			for (int corner : tetrahedron)
				if (solid[corner])
					in[inCount++] = corner;
				else
					out[outCount++] = corner;
			if (!inCount || !outCount)
				continue;
			for (int axis = 0; axis < 3; ++axis)
			{
				for (int i = 0; i < outCount; ++i)
					outward[axis] += float (points[out[i]][axis]) / outCount;
				for (int i = 0; i < inCount; ++i)
					outward[axis] -= float (points[in[i]][axis]) / inCount;
			}
			auto edge = [&] (int a, int b) { return Intersection (points[a], points[b]); };
			if (inCount == 1)
				Triangle (edge (in[0], out[0]), edge (in[0], out[1]), edge (in[0], out[2]), outward, boundary);
			else if (outCount == 1)
				Triangle (edge (in[0], out[0]), edge (in[1], out[0]), edge (in[2], out[0]), outward, boundary);
			else
			{
				const uint32_t a = edge (in[0], out[0]), b = edge (in[0], out[1]), c = edge (in[1], out[0]), d = edge (in[1], out[1]);
				Triangle (a, b, c, outward, boundary);
				Triangle (b, d, c, outward, boundary);
			}
		}
	}
};
} // namespace

void BuildSmoothTerrainMesh (const WorldConfig &config, const std::array<std::shared_ptr<const Chunk>, 27> &chunks, ChunkMesh &mesh)
{
	Builder (config, chunks, mesh).Run (mesh);
}
void BuildSmoothTerrainRegion (
	const WorldConfig &config, const std::array<std::shared_ptr<const Chunk>, 27> &chunks, const std::array<int, 3> &firstCell,
	const std::array<int, 3> &cellCount, ChunkMesh &mesh)
{
	if (!chunks[13])
		throw std::runtime_error ("invalid smooth terrain center snapshot");
	mesh.key = chunks[13]->key;
	Builder (config, chunks, mesh, firstCell, cellCount, true).Run (mesh);
}
} // namespace quiver
