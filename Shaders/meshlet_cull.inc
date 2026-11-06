#include "globals.inc"

// Each workgroup processes one spatial cluster. All its surviving triangles
// append to a single stock material's index interval with one reservation.
layout (local_size_x = 32) in;
layout (push_constant) uniform Params
{
	mat4 mvp; // hybrid mode packs the original four frustum planes here
	vec3 vieworg;
	uint visibility_offset;
	uint first_meshlet;
	uint index_word; // first baked-index word following the three-word run records
	uint flags;
	float pixel_guard; // UV units, one scene pixel on the shorter axis
} pc;
struct meshlet_t
{
	vec3 mins;
	uint material;
	vec4 maxs;
	uvec4 ranges; // first index, index count, first run, run count
};
layout (std430, binding = 0) buffer Commands { draw_indirect_command_t draws[]; };
layout (std430, binding = 1) readonly buffer Meshlets { meshlet_t meshlets[]; };
layout (std430, binding = 2) readonly buffer Visibility { uint visibility[]; };
layout (std430, binding = 3) readonly buffer Bake { uint data[]; };
layout (std430, binding = 4) readonly buffer Surfaces { surface_t surfaces[]; };
layout (std430, binding = 5) writeonly buffer Indices { uint indices[]; };

#ifdef MESHLET_HIZ
// One reverse-Z minimum for each fully covered tile, then the 32² .. 1²
// mip levels. A zero denotes no guaranteed occluder. The optional diagnostic
// counters occupy the tail of the same buffer.
layout (std430, set = 1, binding = 0) buffer HiZ { uint hierarchy[]; };
const uint stats_base = 5461u;
const uint use_depth = 1u, count_stats = 2u, skip_mesh = 4u;
shared vec4 corners_lo[8], corners_hi[8];
shared uint hidden;
#endif

// Match the hardware clip half-spaces. Include enough slack for rounding
// when forming the clip coordinates of a small box far from the origin.
bool outside_frustum (meshlet_t m, vec4 err)
{
	vec3 center = 0.5 * m.mins + 0.5 * m.maxs.xyz;
	vec3 extent = max (center - m.mins, m.maxs.xyz - center);
	vec4 c = pc.mvp * vec4 (center, 1.0);
	for (int axis = 0; axis < 2; ++axis)
	{
		vec3 axis_row = vec3 (pc.mvp[0][axis], pc.mvp[1][axis], pc.mvp[2][axis]);
		vec3 w_row = vec3 (pc.mvp[0].w, pc.mvp[1].w, pc.mvp[2].w);
		float slack = err[axis] + err.w;
		if (c.w + c[axis] + dot (abs (w_row + axis_row), extent) < -slack ||
			c.w - c[axis] + dot (abs (w_row - axis_row), extent) < -slack)
			return true;
	}
	return false;
}

#ifdef MESHLET_HIZ
bool footprint_hidden (ivec2 first, ivec2 last, float nearest)
{
	// First test every intersected cell at a level with a 2x2 footprint.
	// Keep the immediately finer rectangle: if a coarse cell is open in the
	// part *outside* our bounds, a bounded refinement can still reject us.
	int side = 64;
	ivec2 fine_first = first, fine_last = last;
	while (any (greaterThan (last - first, ivec2 (1))))
	{
		fine_first = first;
		fine_last = last;
		side >>= 1;
		first >>= 1;
		last >>= 1;
	}
	int start = (4 * (4096 - side * side)) / 3;
	uint farthest = min (min (hierarchy[start + first.y * side + first.x], hierarchy[start + first.y * side + last.x]),
						 min (hierarchy[start + last.y * side + first.x], hierarchy[start + last.y * side + last.x]));
	if (uintBitsToFloat (farthest) > nearest)
		return true;
	if (side == 64)
		return false;
	side *= 2;
	start = (4 * (4096 - side * side)) / 3;
	for (int y = fine_first.y; y <= fine_last.y; ++y)
		for (int x = fine_first.x; x <= fine_last.x; ++x)
			if (!(uintBitsToFloat (hierarchy[start + y * side + x]) > nearest))
				return false;
	return true;
}

bool bounds_occluded (meshlet_t m, vec4 err, uint lane)
{
	if (lane < 8u)
	{
		vec3 p = mix (m.mins, m.maxs.xyz, bvec3 ((lane & 1u) != 0u, (lane & 2u) != 0u, (lane & 4u) != 0u));
		vec4 clip = pc.mvp * vec4 (p, 1.0);
		// If the box crosses the near plane, contains the camera, or has an
		// unrepresentable projection, never reject it using this hierarchy.
		bool usable = clip.w > err.w && clip.w - err.w > clip.z + err.z;
		vec3 ndc = clip.xyz / max (clip.w, 1e-20);
		vec3 e = (err.xyz + abs (ndc) * err.w) / max (clip.w - err.w, 1e-20) + 1e-6 * (1.0 + abs (ndc));
		usable = usable && !any (isinf (e)) && !any (isnan (e));
		corners_lo[lane] = vec4 (ndc - e, usable ? 1.0 : 0.0);
		corners_hi[lane] = vec4 (ndc + e, 0.0);
	}
	barrier ();
	if (lane == 0u)
	{
		hidden = 0u;
		vec4 lo = corners_lo[0], hi = corners_hi[0];
		for (int i = 1; i < 8; ++i)
		{
			lo = min (lo, corners_lo[i]);
			hi = max (hi, corners_hi[i]);
		}
		if (lo.w > 0.0)
		{
			vec2 uv0 = clamp (lo.xy * 0.5 + 0.5 - pc.pixel_guard, 0.0, 1.0);
			vec2 uv1 = clamp (hi.xy * 0.5 + 0.5 + pc.pixel_guard, 0.0, 1.0);
			ivec2 first = min (ivec2 (floor (uv0 * 64.0)), ivec2 (63));
			ivec2 last = min (ivec2 (floor (uv1 * 64.0)), ivec2 (63));
			// Strict separation keeps self-occluders and depth ties visible.
			if (footprint_hidden (first, last, hi.z + 2e-6))
				hidden = 1u;
		}
	}
	barrier ();
	return hidden != 0u;
}
#endif

shared uint prefix[32];
shared uint out_base, out_count;

void main ()
{
	uint lane = gl_LocalInvocationID.x;
	meshlet_t m = meshlets[pc.first_meshlet + gl_WorkGroupID.x];
#ifdef MESHLET_HIZ
	if ((pc.flags & skip_mesh) != 0u && m.maxs.w > 0.5)
		return;
#else
	if ((pc.flags & 2u) != 0u && m.maxs.w > 0.5)
		return;
#endif
#ifdef MESHLET_HIZ
	bool profile = (pc.flags & count_stats) != 0u && lane == 0u;
	if (profile)
		atomicAdd (hierarchy[stats_base], 1u);
#else
	if ((pc.flags & 1u) != 0u)
	{
		for (uint p = 0; p < 4; ++p)
		{
			vec4 plane = pc.mvp[p];
			vec3 positive = mix (m.mins, m.maxs.xyz, greaterThanEqual (plane.xyz, vec3 (0)));
			if (dot (positive, plane.xyz) < plane.w)
				return;
		}
	}
	else
#endif
	{
		vec3 magnitude = max (abs (m.mins), abs (m.maxs.xyz));
		vec4 err = (abs (pc.mvp[0]) * magnitude.x + abs (pc.mvp[1]) * magnitude.y + abs (pc.mvp[2]) * magnitude.z + abs (pc.mvp[3])) * 2e-6;
		if (outside_frustum (m, err))
		{
#ifdef MESHLET_HIZ
			if (profile)
				atomicAdd (hierarchy[stats_base + 1u], 1u);
#endif
			return;
		}
#ifdef MESHLET_HIZ
		if ((pc.flags & use_depth) != 0u && bounds_occluded (m, err, lane))
		{
			if (profile)
				atomicAdd (hierarchy[stats_base + 2u], 1u);
			return;
		}
#endif
	}
	uint count = 0, first = 0;
	if (lane < m.ranges.w)
	{
		uint r = 3u * (m.ranges.z + lane);
		uint s = data[r];
		// The independent backend uses the BSP only for the source
		// geometric planes. World visibility is entirely view-space.
#ifndef MESHLET_HIZ
		if ((pc.flags & 1u) == 0u || (visibility[pc.visibility_offset + s / 32u] & (1u << (s % 32u))) != 0u)
#endif
		{
			vec3 n = vec3 (surfaces[s].normal_x, surfaces[s].normal_y, surfaces[s].normal_z);
			float dp = dot (n, pc.vieworg);
			bool back = (surfaces[s].packed_tex_edgecount & 0x8000u) != 0u;
			if (!(back ? dp > surfaces[s].dist : dp < surfaces[s].dist))
			{
				first = data[r + 1];
				count = data[r + 2];
			}
		}
	}
	prefix[lane] = count;
	barrier ();
	if (lane == 0)
	{
		uint total = 0;
		for (uint i = 0; i < m.ranges.w; ++i)
		{
			uint size = prefix[i];
			prefix[i] = total;
			total += size;
		}
		out_count = total;
		if (total > 0)
		{
			out_base = draws[m.material].firstIndex + atomicAdd (draws[m.material].indexCount, total);
#ifdef MESHLET_HIZ
			if (profile)
			{
				atomicAdd (hierarchy[stats_base + 3u], 1u);
				atomicAdd (hierarchy[stats_base + 4u], total / 3u);
			}
#endif
		}
	}
	barrier ();
	if (out_count == m.ranges.y)
	{
		// Whole cluster visible: contiguous source and destination on every lane.
		for (uint j = lane; j < out_count; j += gl_WorkGroupSize.x)
			indices[out_base + j] = data[pc.index_word + m.ranges.x + j];
	}
	else if (count > 0)
	{
		// Partial: each visible run owns a disjoint output interval.
		uint dest = out_base + prefix[lane];
		for (uint j = 0; j < count; ++j)
			indices[dest + j] = data[pc.index_word + first + j];
	}
}

