/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "planet_surface.h"
#include <cmath>
#include <bit>
#include <set>
#include <iostream>
#include <stdexcept>
using namespace quiver;
void check (bool value, const char *message)
{
	if (!value)
		throw std::runtime_error (message);
}
int main ()
{
	WorldConfig c;
	c.generator = Generator::Planet;
	c.seed = 9;
	c.planetSettings.version = 1; // Fixed natural-overhang fixture from the original generator.
	c.planetSettings.globalResolution = 32;
	c.planetSettings.extentChunks = 512;
	c.planet = std::make_shared<PlanetGenerator> (c.planetSettings, c.seed);
	check (c.planet->Prepare (), "prepare");
	bool displacement = false, overhang = false;
	for (int x = 17; x < 28 && !overhang; ++x)
		for (int y = 11; y < 24 && !overhang; ++y)
		{
			const auto terrain = c.planet->Sample (x, y, 128, 128);
			auto	   chunk = std::make_shared<Chunk> ();
			chunk->key = {x, y, int64_t (std::floor (terrain.height / ChunkSize))};
			// Fill sampled column's original voxel occupancy, so tests exercise untouched
			// generated geometry rather than treating missing cells as terrain edits.
			for (int z = 0; z < VoxelsPerChunk; ++z)
				if (double (chunk->key.z) * ChunkSize + z * VoxelSize + 4 < c.planet->Height (x, y, 132, 132))
					chunk->cells.Set (16, 16, z, {1, Solid});
			TerrainNeighborhood chunks{};
			chunks[13] = chunk;
			double previous = 1e30;
			for (int dz = -64; dz <= 64; ++dz)
			{
				double local = terrain.height - double (chunk->key.z) * ChunkSize + dz;
				auto   sample = SamplePlanetSurface (c, chunks, {128, 128, local});
				displacement |= std::abs (sample.density + dz) > .1;
				if (sample.density > previous + .05)
					overhang = true;
				previous = sample.density;
				auto alias = std::make_shared<Chunk> (*chunk);
				alias->key.x += c.planetSettings.extentChunks;
				chunks[13] = alias;
				auto repeated = SamplePlanetSurface (c, chunks, {128, 128, local});
				chunks[13] = chunk;
				check (std::abs (sample.density - repeated.density) < 1e-8, "rock density wrap seam");
			}
		}
	check (displacement, "steep slopes have volumetric rock detail");
	check (overhang, "rock field can turn back vertically, supporting natural overhangs");
	// Render bridges follow actual displaced open edges; they remain separate
	// from the collision triangles and reach the neighboring LOD surface.
	ChunkMesh mesh;
	mesh.key = {17, 11, 0};
	mesh.smooth = true;
	mesh.vertices = {{{3, 1, 500}}, {{1022, 1, 520}}, {{3, 1023, 540}}, {{1022, 1023, 560}}};
	mesh.indices = {0, 2, 3, 0, 3, 1};
	const auto originalVertices = mesh.vertices.size (), originalIndices = mesh.indices.size ();
	BuildPlanetTransitions (c, mesh);
	check (mesh.vertices.size () == originalVertices && mesh.indices.size () == originalIndices, "transitions preserve collision geometry");
	uint32_t transitionCount = 0;
	for (uint32_t count : mesh.transitionCounts)
	{
		check (count == 12, "one double-sided bridge for each perimeter edge");
		transitionCount += count;
	}
	check (transitionCount == mesh.transitionIndices.size (), "transition side ranges partition index buffer");
	for (uint32_t index : mesh.transitionIndices)
		check (index < mesh.transitionVertices.size (), "transition indices remain in bounds");
	for (int side = 0; side < 4; ++side)
	{
		const int	axis = side / 2;
		const auto &vertex = mesh.transitionVertices[side * 4 + 2];
		check (vertex.position[axis] == ((side & 1) ? ChunkSize + 16 : -16), "bridge overlaps adjacent coarse tile");
		for (float component : vertex.position)
			check (std::isfinite (component), "finite bridge position");
	}
	check (
		mesh.Bytes () >= (mesh.vertices.size () + mesh.transitionVertices.size ()) * sizeof (MeshVertex) +
							 (mesh.indices.size () + mesh.transitionIndices.size ()) * sizeof (uint32_t),
		"mesh budget includes transition geometry");
	// Collision tiles must be exact subsets of the rendered extraction, including
	// roots/QEF vertices on their negative halo and sharp/material attributes.
	World				world (c);
	const auto			height = world.Config ().planet->Height (17, 11, 128, 128);
	const ChunkKey		key{17, 11, int64_t (std::floor (height / ChunkSize))};
	TerrainNeighborhood neighborhood{};
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
			{
				const ChunkKey adjacent{key.x + x, key.y + y, key.z + z};
				check (world.Require (adjacent), "prepare collision-region neighborhood");
				neighborhood[(z + 1) * 9 + (y + 1) * 3 + x + 1] = world.Find (adjacent);
			}
	ChunkMesh full;
	full.key = key;
	BuildPlanetSurface (world.Config (), neighborhood, full);
	using Triangle = std::array<std::array<uint32_t, 8>, 3>;
	auto triangle = [] (const ChunkMesh &source, size_t i)
	{
		Triangle result;
		for (int corner = 0; corner < 3; ++corner)
			result[corner] = std::bit_cast<std::array<uint32_t, 8>> (source.vertices[source.indices[i + corner]]);
		return result;
	};
	std::set<Triangle> fullTriangles;
	for (size_t i = 0; i < full.indices.size (); i += 3)
		fullTriangles.insert (triangle (full, i));
	const int zCell = std::clamp (int ((height - double (key.z) * ChunkSize) / VoxelSize), 0, VoxelsPerChunk - 1);
	size_t	  compared = 0;
	for (int side = 8; side <= 16; side += 8)
		for (int x : {0, 8, 16, 112})
		{
			ChunkMesh patch;
			BuildPlanetSurfaceRegion (world.Config (), neighborhood, {x, 8, zCell / side * side}, {side, side, side}, patch);
			check (patch.transitionIndices.empty (), "collision region has no render-only skirts");
			for (size_t i = 0; i < patch.indices.size (); i += 3)
			{
				check (fullTriangles.contains (triangle (patch, i)), "collision-region triangle exactly matches full mesh");
				++compared;
			}
		}
	check (compared > 100, "region equivalence fixture exercises surface triangles");
	std::cout << "planet surface checks passed\n";
}
