/* World-space radiance intervals. RC_SET may be overridden when these
 * descriptors are appended to an existing graphics pipeline layout. */
#ifndef QUIVER_RADIANCE_CASCADES_GLSL
#define QUIVER_RADIANCE_CASCADES_GLSL
#ifndef RC_SET
#define RC_SET 0
#endif
const uint RC_LEVELS = 5u;
const float RC_PI = 3.14159265358979323846;
const float RC_NO_HIT = 1e30;
struct RCCascade
{
	vec4 origin_spacing; /* origin xyz; probe spacing */
	vec4 interval;       /* near and far ray distances */
	uvec4 meta;          /* probe side, direction side, brick table offset, brick side */
};
layout (std140, set = RC_SET, binding = 0) uniform RCCascadeData { RCCascade rcLevels[5]; };
/* Fragment receivers never write the cascade. Declaring their descriptors
 * readonly avoids requiring fragmentStoresAndAtomics on the Vulkan device. */
#ifdef RC_RECEIVER_READONLY
#define RC_RAW_ACCESS readonly
#define RC_MERGED_ACCESS readonly
#elif defined(RC_TRACE_WRITEONLY)
/* Trace writes raw data and must only read the previously merged field. */
#define RC_RAW_ACCESS writeonly
#define RC_MERGED_ACCESS readonly
#else
#define RC_RAW_ACCESS
#define RC_MERGED_ACCESS
#endif
layout (std430, set = RC_SET, binding = 1) RC_RAW_ACCESS buffer RCRawIntervals { vec4 rcRaw[]; };
layout (std430, set = RC_SET, binding = 2) RC_MERGED_ACCESS buffer RCMergedIntervals { vec4 rcMerged[]; };
layout (std430, set = RC_SET, binding = 3) RC_RAW_ACCESS buffer RCRawDistances { float rcRawDistance[]; };
layout (std430, set = RC_SET, binding = 4) RC_MERGED_ACCESS buffer RCMergedDistances { float rcMergedDistance[]; };
layout (std430, set = RC_SET, binding = 5) RC_RAW_ACCESS buffer RCRawValidity { uint rcRawValid[]; };
layout (std430, set = RC_SET, binding = 6) RC_MERGED_ACCESS buffer RCMergedValidity { uint rcMergedValid[]; };
#undef RC_RAW_ACCESS
#undef RC_MERGED_ACCESS
/* Immutable snapshots for this submitted frame. UINT_MAX means unallocated. */
layout (std430, set = RC_SET, binding = 7) readonly buffer RCBrickTable { uint rcBrickTable[]; };
layout (std430, set = RC_SET, binding = 8) readonly buffer RCActiveBricks { uvec4 rcActiveBricks[]; };
const uint RC_ABSENT = 0xffffffffu;

uint RC_RecordCount (RCCascade c)
{
	return c.meta.x * c.meta.x * c.meta.x * c.meta.y * c.meta.y;
}
uint RC_RecordIndex (RCCascade c, uvec3 cell, uvec2 direction)
{
	if (c.meta.w == 0u || any (greaterThanEqual (cell, uvec3 (c.meta.x))) ||
		any (greaterThanEqual (direction, uvec2 (c.meta.y)))) return RC_ABSENT;
	uint tableSide = c.meta.x / c.meta.w;
	uvec3 brick = cell / c.meta.w, local = cell % c.meta.w;
	uint tableIndex = c.meta.z + (brick.z * tableSide + brick.y) * tableSide + brick.x;
	if (tableIndex >= rcBrickTable.length ()) return RC_ABSENT;
	uint base = rcBrickTable[tableIndex];
	if (base == RC_ABSENT) return RC_ABSENT;
	uint probe = (local.z * c.meta.w + local.y) * c.meta.w + local.x;
	uint within = probe * c.meta.y * c.meta.y + direction.y * c.meta.y + direction.x;
	if (base > 0xffffffffu - within) return RC_ABSENT;
	return base + within;
}
/* localIndex is in the compact active-brick work list for this level. */
bool RC_DecodeWork (RCCascade c, uint localIndex, uint activeBase, out uvec3 cell, out uvec2 direction, out uint record)
{
	uint directions = c.meta.y * c.meta.y;
	uint brickProbes = c.meta.w * c.meta.w * c.meta.w;
	uint perBrick = brickProbes * directions;
	if (directions == 0u || perBrick == 0u) return false;
	uint brickIndex = activeBase + localIndex / perBrick;
	if (brickIndex < activeBase || brickIndex >= rcActiveBricks.length ()) return false;
	uvec4 brick = rcActiveBricks[brickIndex];
	uint within = localIndex % perBrick;
	uint ray = within % directions;
	uint probe = within / directions;
	direction = uvec2 (ray % c.meta.y, ray / c.meta.y);
	uvec3 inBrick;
	inBrick.x = probe % c.meta.w; probe /= c.meta.w;
	inBrick.y = probe % c.meta.w; inBrick.z = probe / c.meta.w;
	cell = brick.xyz * c.meta.w + inBrick;
	if (any (greaterThanEqual (cell, uvec3 (c.meta.x))) || brick.w > 0xffffffffu - within) return false;
	record = brick.w + within;
	return record < rcRaw.length () && record < rcMerged.length () && record < rcRawDistance.length () &&
		record < rcMergedDistance.length () && record < rcRawValid.length () && record < rcMergedValid.length ();
}
vec3 RC_ProbePosition (RCCascade c, uvec3 cell)
{
	return c.origin_spacing.xyz + (vec3 (cell) + 0.5) * c.origin_spacing.w;
}
/* Equal-solid-angle latitude/azimuth bins; each bin has four children at the
 * next level. The azimuth seam wraps. */
vec3 RC_Direction (uvec2 bin, uint side)
{
	/* Keep each sample inside its angular bin while staggering successive
	 * latitude rows. With a 2x2 base grid, centered azimuths would all lie in
	 * one vertical plane and miss indirect light on perpendicular walls. */
	float azimuth = side > 1u ? ((bin.y & 1u) != 0u ? 0.25 : 0.75) : 0.5;
	vec2 uv = (vec2 (bin) + vec2 (azimuth, 0.5)) / float (side);
	float z = 1.0 - 2.0 * uv.y;
	float radius = sqrt (max (0.0, 1.0 - z * z));
	float phi = 2.0 * RC_PI * uv.x;
	return vec3 (radius * cos (phi), radius * sin (phi), z);
}
vec4 RC_Compose (vec4 nearInterval, vec4 farInterval)
{
	return vec4 (nearInterval.rgb + nearInterval.a * farInterval.rgb, nearInterval.a * farInterval.a);
}
#endif
