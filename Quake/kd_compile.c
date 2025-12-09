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
// kd_compile.c -- Compile .map files to .qkd kd-tree format

#include "quakedef.h"
#include "kd_format.h"
#include "kd_model.h"
#include "kd_svo.h"
#include "jolt_physics.h"

//=============================================================================
// Compiler Data Structures
//=============================================================================

#define KDC_MAX_PLANES      4096
#define KDC_MAX_BRUSHES     1024
#define KDC_MAX_FACES       4096
#define KDC_MAX_VERTS       65536
#define KDC_MAX_ENTITIES    256
#define KDC_MAX_BRUSH_SIDES 32
#define KDC_MAX_FACE_VERTS  32
#define KDC_MAX_TEXTURES    256

#define PLANE_EPSILON       0.001f
#define POINT_EPSILON       0.1f

// Compiler plane
typedef struct cplane_s
{
	vec3_t  normal;
	float   dist;
	int     type;       // 0-2 = axial, 3-5 = near-axial
} cplane_t;

// Compiler brush side (one plane of a brush)
typedef struct cbrushside_s
{
	cplane_t    *plane;           // Canonicalized plane (for deduplication)
	vec3_t      orig_normal;      // Original normal before canonicalization
	float       orig_dist;        // Original dist before canonicalization
	vec3_t      plane_points[3];  // Original 3 points that define the plane
	char        texture[64];
	float       texvecs[2][4];    // Texture coordinate vectors
	float       scale[2];
	float       rotate;
} cbrushside_t;

// Compiler brush (convex solid defined by planes)
typedef struct cbrush_s
{
	int             num_sides;
	cbrushside_t    sides[KDC_MAX_BRUSH_SIDES];
	vec3_t          mins, maxs;
	int             contents;
} cbrush_t;

// Compiler face (polygon)
typedef struct cface_s
{
	cplane_t    *plane;
	int         num_verts;
	vec3_t      *verts;         // Dynamically allocated
	char        texture[64];
	float       texvecs[2][4];
	int         contents;       // Contents behind this face
	vec3_t      mins, maxs;
} cface_t;

// Compiler entity
typedef struct centity_s
{
	char        classname[64];
	vec3_t      origin;
	int         first_brush;
	int         num_brushes;
	char        epairs[4096];   // Raw key-value pairs
} centity_t;

// Kd-tree build node
typedef struct ckdnode_s
{
	int             axis;           // 0=X, 1=Y, 2=Z, -1=leaf
	float           split_dist;
	struct ckdnode_s *children[2];  // [0]=front, [1]=back
	vec3_t          mins, maxs;

	// For leaves
	int             contents;
	int             *face_indices;
	int             num_faces;
} ckdnode_t;

//=============================================================================
// Global Compiler State
//=============================================================================

static cplane_t     map_planes[KDC_MAX_PLANES];
static int          num_map_planes;

static cbrush_t     map_brushes[KDC_MAX_BRUSHES];
static int          num_map_brushes;

static cface_t      map_faces[KDC_MAX_FACES];
static int          num_map_faces;

static centity_t    map_entities[KDC_MAX_ENTITIES];
static int          num_map_entities;

// static vec3_t       map_verts[MAX_MAP_VERTS];  // Not used currently
// static int          num_map_verts;

static char         map_textures[KDC_MAX_TEXTURES][64];
static int          num_map_textures;

static char         *map_data;
static char         *map_ptr;

//=============================================================================
// Utility Functions
//=============================================================================

static void ClearCompiler (void)
{
	// Free face vertices
	for (int i = 0; i < num_map_faces; i++)
	{
		if (map_faces[i].verts)
			free(map_faces[i].verts);
	}

	num_map_planes = 0;
	num_map_brushes = 0;
	num_map_faces = 0;
	num_map_entities = 0;
	num_map_textures = 0;

	memset(map_faces, 0, sizeof(map_faces));
}

static int PlaneTypeForNormal (vec3_t normal)
{
	if (normal[0] == 1.0f || normal[0] == -1.0f)
		return 0;
	if (normal[1] == 1.0f || normal[1] == -1.0f)
		return 1;
	if (normal[2] == 1.0f || normal[2] == -1.0f)
		return 2;

	float ax = fabs(normal[0]);
	float ay = fabs(normal[1]);
	float az = fabs(normal[2]);

	if (ax >= ay && ax >= az)
		return 3;
	if (ay >= ax && ay >= az)
		return 4;
	return 5;
}

static cplane_t *FindOrCreatePlane (vec3_t normal, float dist, qboolean *was_flipped)
{
	// Normalize the plane (ensure normal points in consistent direction)
	vec3_t n;
	float d = dist;
	VectorCopy(normal, n);
	qboolean flipped = false;

	float len = sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
	if (len < 0.001f)
		return NULL;

	n[0] /= len;
	n[1] /= len;
	n[2] /= len;
	d /= len;

	// Make sure normal points towards positive (canonical form)
	if (n[0] < -PLANE_EPSILON ||
	    (fabs(n[0]) < PLANE_EPSILON && n[1] < -PLANE_EPSILON) ||
	    (fabs(n[0]) < PLANE_EPSILON && fabs(n[1]) < PLANE_EPSILON && n[2] < -PLANE_EPSILON))
	{
		n[0] = -n[0];
		n[1] = -n[1];
		n[2] = -n[2];
		d = -d;
		flipped = true;
	}

	if (was_flipped)
		*was_flipped = flipped;

	// Search for existing plane
	for (int i = 0; i < num_map_planes; i++)
	{
		if (fabs(map_planes[i].normal[0] - n[0]) < PLANE_EPSILON &&
		    fabs(map_planes[i].normal[1] - n[1]) < PLANE_EPSILON &&
		    fabs(map_planes[i].normal[2] - n[2]) < PLANE_EPSILON &&
		    fabs(map_planes[i].dist - d) < PLANE_EPSILON)
		{
			return &map_planes[i];
		}
	}

	// Create new plane
	if (num_map_planes >= KDC_MAX_PLANES)
	{
		Con_Printf("ERROR: KDC_MAX_PLANES\n");
		return NULL;
	}

	cplane_t *plane = &map_planes[num_map_planes++];
	VectorCopy(n, plane->normal);
	plane->dist = d;
	plane->type = PlaneTypeForNormal(n);

	return plane;
}

static int FindOrCreateTexture (const char *name)
{
	for (int i = 0; i < num_map_textures; i++)
	{
		if (!strcmp(map_textures[i], name))
			return i;
	}

	if (num_map_textures >= KDC_MAX_TEXTURES)
	{
		Con_Printf("ERROR: KDC_MAX_TEXTURES\n");
		return 0;
	}

	q_strlcpy(map_textures[num_map_textures], name, 64);
	return num_map_textures++;
}

//=============================================================================
// .map File Parser
//=============================================================================

static void SkipWhitespace (void)
{
	while (*map_ptr && (*map_ptr == ' ' || *map_ptr == '\t' || *map_ptr == '\r' || *map_ptr == '\n'))
		map_ptr++;
}

static void SkipLine (void)
{
	while (*map_ptr && *map_ptr != '\n')
		map_ptr++;
	if (*map_ptr == '\n')
		map_ptr++;
}

static qboolean ParseToken (char *token, int maxlen)
{
	SkipWhitespace();

	if (!*map_ptr)
		return false;

	// Handle quoted strings
	if (*map_ptr == '"')
	{
		map_ptr++;
		int i = 0;
		while (*map_ptr && *map_ptr != '"' && i < maxlen - 1)
			token[i++] = *map_ptr++;
		token[i] = '\0';
		if (*map_ptr == '"')
			map_ptr++;
		return true;
	}

	// Handle regular tokens
	int i = 0;
	while (*map_ptr && *map_ptr != ' ' && *map_ptr != '\t' &&
	       *map_ptr != '\r' && *map_ptr != '\n' && *map_ptr != '"' &&
	       i < maxlen - 1)
	{
		token[i++] = *map_ptr++;
	}
	token[i] = '\0';

	return i > 0;
}

static qboolean ParseFloat (float *f)
{
	char token[64];
	if (!ParseToken(token, sizeof(token)))
		return false;
	*f = atof(token);
	return true;
}

/* Not currently used
static qboolean ParseInt (int *i)
{
	char token[64];
	if (!ParseToken(token, sizeof(token)))
		return false;
	*i = atoi(token);
	return true;
}
*/

// Parse a brush plane: ( x1 y1 z1 ) ( x2 y2 z2 ) ( x3 y3 z3 ) texture xoff yoff rot xscale yscale
static qboolean ParseBrushSide (cbrushside_t *side)
{
	vec3_t p1, p2, p3;
	char token[64];

	// Parse three points
	if (!ParseToken(token, sizeof(token)) || strcmp(token, "("))
		return false;
	if (!ParseFloat(&p1[0]) || !ParseFloat(&p1[1]) || !ParseFloat(&p1[2]))
		return false;
	if (!ParseToken(token, sizeof(token)) || strcmp(token, ")"))
		return false;

	if (!ParseToken(token, sizeof(token)) || strcmp(token, "("))
		return false;
	if (!ParseFloat(&p2[0]) || !ParseFloat(&p2[1]) || !ParseFloat(&p2[2]))
		return false;
	if (!ParseToken(token, sizeof(token)) || strcmp(token, ")"))
		return false;

	if (!ParseToken(token, sizeof(token)) || strcmp(token, "("))
		return false;
	if (!ParseFloat(&p3[0]) || !ParseFloat(&p3[1]) || !ParseFloat(&p3[2]))
		return false;
	if (!ParseToken(token, sizeof(token)) || strcmp(token, ")"))
		return false;

	// Calculate plane from three points
	vec3_t v1, v2, normal;
	VectorSubtract(p2, p1, v1);
	VectorSubtract(p3, p1, v2);
	CrossProduct(v1, v2, normal);

	float len = VectorLength(normal);
	if (len < 0.001f)
	{
		Con_Printf("WARNING: Degenerate brush plane\n");
		return false;
	}

	VectorScale(normal, 1.0f / len, normal);

	// Negate normal so planes point OUTWARD from brush (Quake convention)
	// Map files typically have winding that produces inward-pointing normals
	normal[0] = -normal[0];
	normal[1] = -normal[1];
	normal[2] = -normal[2];

	float dist = DotProduct(p1, normal);

	// Store original plane (before canonicalization) for clipping
	VectorCopy(normal, side->orig_normal);
	side->orig_dist = dist;

	// Store original points for brush center calculation
	VectorCopy(p1, side->plane_points[0]);
	VectorCopy(p2, side->plane_points[1]);
	VectorCopy(p3, side->plane_points[2]);

	side->plane = FindOrCreatePlane(normal, dist, NULL);
	if (!side->plane)
		return false;

	// Parse texture name
	if (!ParseToken(side->texture, sizeof(side->texture)))
		return false;

	// Parse texture parameters (xoff yoff rotation xscale yscale)
	float xoff, yoff, rotation, xscale, yscale;
	if (!ParseFloat(&xoff) || !ParseFloat(&yoff) || !ParseFloat(&rotation) ||
	    !ParseFloat(&xscale) || !ParseFloat(&yscale))
	{
		// Default values if not present
		xoff = yoff = rotation = 0;
		xscale = yscale = 1;
	}

	side->texvecs[0][3] = xoff;
	side->texvecs[1][3] = yoff;
	side->scale[0] = (xscale != 0) ? xscale : 1;
	side->scale[1] = (yscale != 0) ? yscale : 1;
	side->rotate = rotation;

	// Skip to end of line (handle any extra parameters)
	SkipLine();

	return true;
}

static qboolean ParseBrush (cbrush_t *brush)
{
	char token[64];

	memset(brush, 0, sizeof(*brush));
	brush->contents = CONTENTS_SOLID;

	// Expect opening brace
	if (!ParseToken(token, sizeof(token)) || strcmp(token, "{"))
		return false;

	// Parse brush sides until closing brace
	while (1)
	{
		SkipWhitespace();

		// Check for closing brace
		if (*map_ptr == '}')
		{
			map_ptr++;
			break;
		}

		// Check for comment
		if (*map_ptr == '/' && *(map_ptr + 1) == '/')
		{
			SkipLine();
			continue;
		}

		// Parse brush side
		if (*map_ptr == '(')
		{
			if (brush->num_sides >= KDC_MAX_BRUSH_SIDES)
			{
				Con_Printf("ERROR: KDC_MAX_BRUSH_SIDES\n");
				return false;
			}

			if (!ParseBrushSide(&brush->sides[brush->num_sides]))
			{
				Con_Printf("ERROR: Failed to parse brush side\n");
				return false;
			}

			brush->num_sides++;
		}
		else
		{
			// Unknown content, skip line
			SkipLine();
		}
	}

	return brush->num_sides >= 4;  // Need at least 4 sides for a valid brush
}

static qboolean ParseEntity (centity_t *entity)
{
	char token[64];
	char key[64];
	char value[256];

	memset(entity, 0, sizeof(*entity));
	entity->first_brush = num_map_brushes;

	// Expect opening brace
	if (!ParseToken(token, sizeof(token)) || strcmp(token, "{"))
		return false;

	// Parse key-value pairs and brushes
	while (1)
	{
		SkipWhitespace();

		// Check for closing brace
		if (*map_ptr == '}')
		{
			map_ptr++;
			break;
		}

		// Check for comment
		if (*map_ptr == '/' && *(map_ptr + 1) == '/')
		{
			SkipLine();
			continue;
		}

		// Check for nested brush
		if (*map_ptr == '{')
		{
			if (num_map_brushes >= KDC_MAX_BRUSHES)
			{
				Con_Printf("ERROR: KDC_MAX_BRUSHES\n");
				return false;
			}

			if (!ParseBrush(&map_brushes[num_map_brushes]))
			{
				Con_Printf("ERROR: Failed to parse brush\n");
				return false;
			}

			num_map_brushes++;
			entity->num_brushes++;
			continue;
		}

		// Parse key-value pair
		if (*map_ptr == '"')
		{
			if (!ParseToken(key, sizeof(key)))
				break;
			if (!ParseToken(value, sizeof(value)))
				break;

			// Store special keys
			if (!strcmp(key, "classname"))
				q_strlcpy(entity->classname, value, sizeof(entity->classname));
			else if (!strcmp(key, "origin"))
				sscanf(value, "%f %f %f", &entity->origin[0], &entity->origin[1], &entity->origin[2]);

			// Append to epairs
			size_t len = strlen(entity->epairs);
			if (len + strlen(key) + strlen(value) + 8 < sizeof(entity->epairs))
			{
				strcat(entity->epairs, "\"");
				strcat(entity->epairs, key);
				strcat(entity->epairs, "\" \"");
				strcat(entity->epairs, value);
				strcat(entity->epairs, "\"\n");
			}
		}
		else
		{
			// Unknown content, skip line
			SkipLine();
		}
	}

	return true;
}

static qboolean ParseMapFile (const char *filename)
{
	// Load file
	map_data = (char *)COM_LoadFile(filename, NULL);
	if (!map_data)
	{
		Con_Printf("ERROR: Couldn't load %s\n", filename);
		return false;
	}

	map_ptr = map_data;

	// Parse entities
	while (1)
	{
		SkipWhitespace();

		if (!*map_ptr)
			break;

		// Check for comment
		if (*map_ptr == '/' && *(map_ptr + 1) == '/')
		{
			SkipLine();
			continue;
		}

		// Parse entity
		if (*map_ptr == '{')
		{
			if (num_map_entities >= KDC_MAX_ENTITIES)
			{
				Con_Printf("ERROR: KDC_MAX_ENTITIES\n");
				break;
			}

			if (!ParseEntity(&map_entities[num_map_entities]))
			{
				Con_Printf("ERROR: Failed to parse entity\n");
				break;
			}

			num_map_entities++;
		}
		else
		{
			map_ptr++;
		}
	}

	Mem_Free(map_data);
	map_data = NULL;

	Con_Printf("Parsed %d entities, %d brushes\n", num_map_entities, num_map_brushes);

	return num_map_entities > 0;
}

//=============================================================================
// Brush to Polygon Conversion
//=============================================================================

// Clip a polygon by a plane (using raw normal/dist)
// keep_front: if true, keep front side (d > 0); if false, keep back side (d < 0)
static int ClipPolyByPlaneRaw (vec3_t *verts, int num_verts, vec3_t normal, float dist, vec3_t *out, qboolean keep_front)
{
	static float dists[KDC_MAX_FACE_VERTS + 1];
	static int sides[KDC_MAX_FACE_VERTS + 1];
	int counts[3] = {0, 0, 0};
	int num_out = 0;

	// Which side to keep: 0 = front (d > 0), 1 = back (d < 0)
	int keep_side = keep_front ? 0 : 1;
	int clip_side = keep_front ? 1 : 0;

	// Bounds check
	if (num_verts > KDC_MAX_FACE_VERTS)
		num_verts = KDC_MAX_FACE_VERTS;

	// Classify vertices
	for (int i = 0; i < num_verts; i++)
	{
		float d = DotProduct(verts[i], normal) - dist;
		dists[i] = d;

		if (d > POINT_EPSILON)
			sides[i] = 0;  // Front
		else if (d < -POINT_EPSILON)
			sides[i] = 1;  // Back
		else
			sides[i] = 2;  // On plane

		counts[sides[i]]++;
	}

	// Wrap around
	dists[num_verts] = dists[0];
	sides[num_verts] = sides[0];

	// All on clip side = clipped away entirely
	if (!counts[keep_side] && !counts[2])
		return 0;

	// All on keep side or on plane = keep as is
	if (!counts[clip_side])
	{
		int copy_count = (num_verts > KDC_MAX_FACE_VERTS) ? KDC_MAX_FACE_VERTS : num_verts;
		for (int i = 0; i < copy_count; i++)
			VectorCopy(verts[i], out[i]);
		return copy_count;
	}

	// Split the polygon - keep vertices on the keep side
	for (int i = 0; i < num_verts; i++)
	{
		if (sides[i] == keep_side || sides[i] == 2)  // Keep side or on-plane
		{
			if (num_out >= KDC_MAX_FACE_VERTS - 1)
				break;
			VectorCopy(verts[i], out[num_out]);
			num_out++;
		}

		if (sides[i] == 2 || sides[i + 1] == 2)
			continue;

		if (sides[i] != sides[i + 1])
		{
			if (num_out >= KDC_MAX_FACE_VERTS - 1)
				break;
			// Generate split point
			float frac = dists[i] / (dists[i] - dists[i + 1]);
			vec3_t *p1 = &verts[i];
			vec3_t *p2 = &verts[(i + 1) % num_verts];

			for (int j = 0; j < 3; j++)
				out[num_out][j] = (*p1)[j] + frac * ((*p2)[j] - (*p1)[j]);
			num_out++;
		}
	}

	return num_out;
}

// Create a large base polygon for a plane (using raw normal/dist)
static int CreateBaseWindingRaw (vec3_t normal, float dist, vec3_t *verts)
{
	// Find the major axis
	int axis = 0;
	float max = fabs(normal[0]);
	if (fabs(normal[1]) > max) { max = fabs(normal[1]); axis = 1; }
	if (fabs(normal[2]) > max) { axis = 2; }

	// Create two vectors perpendicular to the normal
	vec3_t up = {0, 0, 0};
	if (axis == 0 || axis == 1)
		up[2] = 1;
	else
		up[0] = 1;

	vec3_t right, forward;
	CrossProduct(up, normal, right);
	float len = VectorLength(right);
	if (len > 0.001f)
		VectorScale(right, 1.0f / len, right);
	CrossProduct(normal, right, forward);

	// Create a large quad centered on the plane
	vec3_t center;
	VectorScale(normal, dist, center);

	// Use a reasonable size - 8192 is typical Quake map bounds
	float size = 8192.0f;

	VectorMA(center, -size, right, verts[0]);
	VectorMA(verts[0], -size, forward, verts[0]);

	VectorMA(center, size, right, verts[1]);
	VectorMA(verts[1], -size, forward, verts[1]);

	VectorMA(center, size, right, verts[2]);
	VectorMA(verts[2], size, forward, verts[2]);

	VectorMA(center, -size, right, verts[3]);
	VectorMA(verts[3], size, forward, verts[3]);

	return 4;
}

// Generate texture coordinates for a face
static void GenerateTexCoords (cface_t *face, cbrushside_t *side)
{
	// Simple planar projection based on the face normal
	vec3_t *n = &face->plane->normal;

	// Find the two axes to use for texcoords
	int u_axis, v_axis;
	if (fabs((*n)[0]) >= fabs((*n)[1]) && fabs((*n)[0]) >= fabs((*n)[2]))
	{
		u_axis = 1;
		v_axis = 2;
	}
	else if (fabs((*n)[1]) >= fabs((*n)[0]) && fabs((*n)[1]) >= fabs((*n)[2]))
	{
		u_axis = 0;
		v_axis = 2;
	}
	else
	{
		u_axis = 0;
		v_axis = 1;
	}

	// Set up texture vectors (1/64 scale by default)
	float scale = 1.0f / 64.0f;
	memset(face->texvecs, 0, sizeof(face->texvecs));
	face->texvecs[0][u_axis] = scale / side->scale[0];
	face->texvecs[1][v_axis] = scale / side->scale[1];
	face->texvecs[0][3] = side->texvecs[0][3] * scale;
	face->texvecs[1][3] = side->texvecs[1][3] * scale;
}

// Convert a brush to faces
static void BrushToFaces (cbrush_t *brush)
{
	static vec3_t temp_verts[KDC_MAX_FACE_VERTS];
	static vec3_t clipped_verts[KDC_MAX_FACE_VERTS];
	static int brush_num = 0;

	// Compute brush center by averaging the 3 points that define each plane
	// These points are ON the brush surface, so their average is inside
	vec3_t brush_center = {0, 0, 0};
	int num_points = 0;
	for (int i = 0; i < brush->num_sides; i++)
	{
		VectorAdd(brush_center, brush->sides[i].plane_points[0], brush_center);
		VectorAdd(brush_center, brush->sides[i].plane_points[1], brush_center);
		VectorAdd(brush_center, brush->sides[i].plane_points[2], brush_center);
		num_points += 3;
	}
	if (num_points > 0)
		VectorScale(brush_center, 1.0f / num_points, brush_center);

	Con_DPrintf("Brush %d center: (%.1f, %.1f, %.1f)\n", brush_num,
	            brush_center[0], brush_center[1], brush_center[2]);

	for (int i = 0; i < brush->num_sides; i++)
	{
		cbrushside_t *side = &brush->sides[i];

		// Skip if texture is "skip" or similar
		if (!strcmp(side->texture, "skip") || !strcmp(side->texture, "hint") ||
		    !strcmp(side->texture, "trigger"))
			continue;

		// Create base winding for this plane - use ORIGINAL plane (before canonicalization)
		int num_verts = CreateBaseWindingRaw(side->orig_normal, side->orig_dist, temp_verts);

		Con_DPrintf("Brush %d Side %d: normal=(%.2f,%.2f,%.2f) dist=%.1f, base_verts=%d\n",
		            brush_num, i, side->orig_normal[0], side->orig_normal[1], side->orig_normal[2],
		            side->orig_dist, num_verts);

		// Debug: track clipping
		int clipped_by = -1;

		// Clip by all other planes
		for (int j = 0; j < brush->num_sides; j++)
		{
			if (i == j)
				continue;

			cbrushside_t *clip_side = &brush->sides[j];

			// Determine which side of the clip plane the brush center is on
			// signed_dist > 0 means center is in FRONT of the plane
			float center_dist = DotProduct(brush_center, clip_side->orig_normal) - clip_side->orig_dist;
			qboolean keep_front = (center_dist > 0);

			// Use original (non-canonicalized) plane, keep the side containing brush center
			num_verts = ClipPolyByPlaneRaw(temp_verts, num_verts, clip_side->orig_normal, clip_side->orig_dist, clipped_verts, keep_front);

			if (num_verts < 3)
			{
				clipped_by = j;
				break;
			}

			// Clamp to max and copy back
			if (num_verts > KDC_MAX_FACE_VERTS)
				num_verts = KDC_MAX_FACE_VERTS;
			for (int k = 0; k < num_verts; k++)
				VectorCopy(clipped_verts[k], temp_verts[k]);
		}

		if (num_verts < 3)
		{
			Con_DPrintf("  Side %d: degenerate (clipped away by side %d), normal=(%.2f,%.2f,%.2f)\n",
			            i, clipped_by, side->orig_normal[0], side->orig_normal[1], side->orig_normal[2]);
			continue;
		}

		// Create face
		if (num_map_faces >= KDC_MAX_FACES)
		{
			Con_Printf("ERROR: KDC_MAX_FACES\n");
			return;
		}

		cface_t *face = &map_faces[num_map_faces++];
		face->plane = side->plane;
		face->num_verts = num_verts;
		face->verts = (vec3_t *)malloc(num_verts * sizeof(vec3_t));
		memcpy(face->verts, temp_verts, num_verts * sizeof(vec3_t));
		q_strlcpy(face->texture, side->texture, sizeof(face->texture));
		face->contents = brush->contents;

		// Calculate face bounds
		VectorCopy(face->verts[0], face->mins);
		VectorCopy(face->verts[0], face->maxs);
		for (int v = 1; v < num_verts; v++)
		{
			for (int a = 0; a < 3; a++)
			{
				if (face->verts[v][a] < face->mins[a])
					face->mins[a] = face->verts[v][a];
				if (face->verts[v][a] > face->maxs[a])
					face->maxs[a] = face->verts[v][a];
			}
		}

		// Generate texture coordinates
		GenerateTexCoords(face, side);
	}

	brush_num++;
}

//=============================================================================
// Kd-Tree Builder
//=============================================================================

#define MIN_FACES_PER_LEAF  4
#define MAX_TREE_DEPTH      32

static ckdnode_t *kd_nodes;
static int num_kd_nodes;
static int max_kd_nodes;

static ckdnode_t *AllocKdNode (void)
{
	if (num_kd_nodes >= max_kd_nodes)
	{
		max_kd_nodes = max_kd_nodes ? max_kd_nodes * 2 : 1024;
		kd_nodes = (ckdnode_t *)realloc(kd_nodes, max_kd_nodes * sizeof(ckdnode_t));
	}
	ckdnode_t *node = &kd_nodes[num_kd_nodes++];
	memset(node, 0, sizeof(*node));
	return node;
}

// Surface Area Heuristic for split selection
static float EvaluateSplit (int *face_indices, int num_faces, int axis, float split_dist,
                            vec3_t bounds_mins, vec3_t bounds_maxs)
{
	int front_count = 0, back_count = 0, split_count = 0;

	for (int i = 0; i < num_faces; i++)
	{
		cface_t *face = &map_faces[face_indices[i]];

		if (face->mins[axis] >= split_dist)
			front_count++;
		else if (face->maxs[axis] <= split_dist)
			back_count++;
		else
		{
			split_count++;
			front_count++;
			back_count++;
		}
	}

	// Calculate child surface areas

	// Surface area of children
	vec3_t front_size, back_size;
	VectorSubtract(bounds_maxs, bounds_mins, front_size);
	VectorSubtract(bounds_maxs, bounds_mins, back_size);
	front_size[axis] = bounds_maxs[axis] - split_dist;
	back_size[axis] = split_dist - bounds_mins[axis];

	float front_area = 2 * (front_size[0]*front_size[1] + front_size[1]*front_size[2] + front_size[0]*front_size[2]);
	float back_area = 2 * (back_size[0]*back_size[1] + back_size[1]*back_size[2] + back_size[0]*back_size[2]);

	// SAH cost
	float cost = 1.0f + (front_area * front_count + back_area * back_count);

	return cost;
}

// Find best split for a set of faces
static qboolean FindBestSplit (int *face_indices, int num_faces, vec3_t bounds_mins, vec3_t bounds_maxs,
                               int *best_axis, float *best_dist)
{
	float best_cost = num_faces * 2;  // Cost of not splitting
	qboolean found = false;

	for (int axis = 0; axis < 3; axis++)
	{
		float size = bounds_maxs[axis] - bounds_mins[axis];
		if (size < 1.0f)
			continue;

		// Try several split positions
		for (int i = 1; i < 8; i++)
		{
			float t = i / 8.0f;
			float split_dist = bounds_mins[axis] + t * size;

			float cost = EvaluateSplit(face_indices, num_faces, axis, split_dist, bounds_mins, bounds_maxs);

			if (cost < best_cost)
			{
				best_cost = cost;
				*best_axis = axis;
				*best_dist = split_dist;
				found = true;
			}
		}

		// Also try face boundaries
		for (int i = 0; i < num_faces && i < 32; i++)
		{
			cface_t *face = &map_faces[face_indices[i]];

			float splits[2] = { face->mins[axis], face->maxs[axis] };
			for (int s = 0; s < 2; s++)
			{
				if (splits[s] <= bounds_mins[axis] || splits[s] >= bounds_maxs[axis])
					continue;

				float cost = EvaluateSplit(face_indices, num_faces, axis, splits[s], bounds_mins, bounds_maxs);

				if (cost < best_cost)
				{
					best_cost = cost;
					*best_axis = axis;
					*best_dist = splits[s];
					found = true;
				}
			}
		}
	}

	return found;
}

// Recursively build kd-tree
static ckdnode_t *BuildKdTree (int *face_indices, int num_faces, vec3_t bounds_mins, vec3_t bounds_maxs, int depth)
{
	ckdnode_t *node = AllocKdNode();
	VectorCopy(bounds_mins, node->mins);
	VectorCopy(bounds_maxs, node->maxs);

	// Check for leaf conditions
	if (num_faces <= MIN_FACES_PER_LEAF || depth >= MAX_TREE_DEPTH)
	{
		node->axis = -1;  // Leaf
		node->contents = num_faces > 0 ? CONTENTS_SOLID : CONTENTS_EMPTY;
		node->num_faces = num_faces;
		if (num_faces > 0)
		{
			node->face_indices = (int *)malloc(num_faces * sizeof(int));
			memcpy(node->face_indices, face_indices, num_faces * sizeof(int));
		}
		return node;
	}

	// Find best split
	int best_axis;
	float best_dist;

	if (!FindBestSplit(face_indices, num_faces, bounds_mins, bounds_maxs, &best_axis, &best_dist))
	{
		// Couldn't find good split, make leaf
		node->axis = -1;
		node->contents = num_faces > 0 ? CONTENTS_SOLID : CONTENTS_EMPTY;
		node->num_faces = num_faces;
		if (num_faces > 0)
		{
			node->face_indices = (int *)malloc(num_faces * sizeof(int));
			memcpy(node->face_indices, face_indices, num_faces * sizeof(int));
		}
		return node;
	}

	node->axis = best_axis;
	node->split_dist = best_dist;

	// Partition faces
	int *front_faces = (int *)malloc(num_faces * sizeof(int));
	int *back_faces = (int *)malloc(num_faces * sizeof(int));
	int num_front = 0, num_back = 0;

	for (int i = 0; i < num_faces; i++)
	{
		cface_t *face = &map_faces[face_indices[i]];

		if (face->maxs[best_axis] <= best_dist)
			back_faces[num_back++] = face_indices[i];
		else if (face->mins[best_axis] >= best_dist)
			front_faces[num_front++] = face_indices[i];
		else
		{
			// Face spans split - add to both
			front_faces[num_front++] = face_indices[i];
			back_faces[num_back++] = face_indices[i];
		}
	}

	// Build children
	vec3_t front_mins, front_maxs, back_mins, back_maxs;
	VectorCopy(bounds_mins, front_mins);
	VectorCopy(bounds_maxs, front_maxs);
	VectorCopy(bounds_mins, back_mins);
	VectorCopy(bounds_maxs, back_maxs);

	front_mins[best_axis] = best_dist;
	back_maxs[best_axis] = best_dist;

	node->children[0] = BuildKdTree(front_faces, num_front, front_mins, front_maxs, depth + 1);
	node->children[1] = BuildKdTree(back_faces, num_back, back_mins, back_maxs, depth + 1);

	free(front_faces);
	free(back_faces);

	return node;
}

//=============================================================================
// .qkd File Writer
//=============================================================================

static void WriteQKDFile (const char *filename)
{
	char fullpath[MAX_OSPATH];
	q_snprintf(fullpath, sizeof(fullpath), "%s/%s", com_gamedir, filename);

	FILE *f = fopen(fullpath, "wb");
	if (!f)
	{
		Con_Printf("ERROR: Couldn't create %s\n", fullpath);
		return;
	}

	// Prepare header
	dkdheader_t header;
	memset(&header, 0, sizeof(header));
	header.magic = KDFORMAT_MAGIC;
	header.version = KDFORMAT_VERSION;

	int offset = sizeof(dkdheader_t);

	// Build entity string
	char *entities = (char *)malloc(65536);
	entities[0] = '\0';
	size_t ent_pos = 0;
	for (int i = 0; i < num_map_entities; i++)
	{
		size_t needed = strlen(map_entities[i].epairs) + 4;
		if (ent_pos + needed < 65536)
		{
			strcpy(entities + ent_pos, "{\n");
			ent_pos += 2;
			strcpy(entities + ent_pos, map_entities[i].epairs);
			ent_pos += strlen(map_entities[i].epairs);
			strcpy(entities + ent_pos, "}\n");
			ent_pos += 2;
		}
	}
	int entities_len = (int)ent_pos + 1;

	// Count nodes and leaves
	int node_count = 0, leaf_count = 0;
	for (int i = 0; i < num_kd_nodes; i++)
	{
		if (kd_nodes[i].axis >= 0)
			node_count++;
		else
			leaf_count++;
	}

	// Entities lump
	header.lumps[KD_LUMP_ENTITIES].offset = offset;
	header.lumps[KD_LUMP_ENTITIES].length = entities_len;
	offset += entities_len;

	// Planes lump
	header.lumps[KD_LUMP_PLANES].offset = offset;
	header.lumps[KD_LUMP_PLANES].length = num_map_planes * sizeof(dkdplane_t);
	offset += header.lumps[KD_LUMP_PLANES].length;

	// Vertices lump (all face vertices)
	int total_verts = 0;
	for (int i = 0; i < num_map_faces; i++)
		total_verts += map_faces[i].num_verts;

	header.lumps[KD_LUMP_VERTICES].offset = offset;
	header.lumps[KD_LUMP_VERTICES].length = total_verts * sizeof(dkdvertex_t);
	offset += header.lumps[KD_LUMP_VERTICES].length;

	// Nodes lump
	header.lumps[KD_LUMP_KDNODES].offset = offset;
	header.lumps[KD_LUMP_KDNODES].length = node_count * sizeof(dkdnode_t);
	offset += header.lumps[KD_LUMP_KDNODES].length;

	// Leaves lump
	header.lumps[KD_LUMP_KDLEAVES].offset = offset;
	header.lumps[KD_LUMP_KDLEAVES].length = leaf_count * sizeof(dkdleaf_t);
	offset += header.lumps[KD_LUMP_KDLEAVES].length;

	// Surfaces lump
	header.lumps[KD_LUMP_SURFACES].offset = offset;
	header.lumps[KD_LUMP_SURFACES].length = num_map_faces * sizeof(dkdsurface_t);
	offset += header.lumps[KD_LUMP_SURFACES].length;

	// Models lump (just worldspawn for now)
	header.lumps[KD_LUMP_MODELS].offset = offset;
	header.lumps[KD_LUMP_MODELS].length = sizeof(dkdmodel_t);
	offset += header.lumps[KD_LUMP_MODELS].length;

	// Write header
	fwrite(&header, sizeof(header), 1, f);

	// Write entities
	fwrite(entities, entities_len, 1, f);

	// Write planes
	for (int i = 0; i < num_map_planes; i++)
	{
		dkdplane_t dp;
		VectorCopy(map_planes[i].normal, dp.normal);
		dp.dist = map_planes[i].dist;
		dp.type = map_planes[i].type;
		fwrite(&dp, sizeof(dp), 1, f);
	}

	// Write vertices and build surface data
	int vert_offset = 0;
	for (int i = 0; i < num_map_faces; i++)
	{
		cface_t *face = &map_faces[i];
		for (int v = 0; v < face->num_verts; v++)
		{
			dkdvertex_t dv;
			VectorCopy(face->verts[v], dv.position);
			fwrite(&dv, sizeof(dv), 1, f);
		}
	}

	// Write nodes (need to convert tree structure to array with indices)
	// This is simplified - a proper implementation would linearize the tree
	int *node_indices = (int *)calloc(num_kd_nodes, sizeof(int));
	int node_idx = 0, leaf_idx = 0;

	// First pass: assign indices
	for (int i = 0; i < num_kd_nodes; i++)
	{
		if (kd_nodes[i].axis >= 0)
			node_indices[i] = node_idx++;
		else
			node_indices[i] = ~leaf_idx++;
	}

	// Write nodes
	for (int i = 0; i < num_kd_nodes; i++)
	{
		if (kd_nodes[i].axis < 0)
			continue;

		dkdnode_t dn;
		dn.axis = kd_nodes[i].axis;
		dn.pad[0] = dn.pad[1] = dn.pad[2] = 0;
		dn.split_dist = kd_nodes[i].split_dist;

		// Find children indices
		dn.children[0] = 0;
		dn.children[1] = 0;
		for (int j = 0; j < num_kd_nodes; j++)
		{
			if (&kd_nodes[j] == kd_nodes[i].children[0])
				dn.children[0] = node_indices[j];
			if (&kd_nodes[j] == kd_nodes[i].children[1])
				dn.children[1] = node_indices[j];
		}

		VectorCopy(kd_nodes[i].mins, dn.mins);
		VectorCopy(kd_nodes[i].maxs, dn.maxs);
		fwrite(&dn, sizeof(dn), 1, f);
	}

	// Write leaves
	for (int i = 0; i < num_kd_nodes; i++)
	{
		if (kd_nodes[i].axis >= 0)
			continue;

		dkdleaf_t dl;
		dl.contents = kd_nodes[i].contents;
		dl.zone = 0;
		dl.first_surface = 0;  // Would need proper surface assignment
		dl.num_surfaces = kd_nodes[i].num_faces;
		VectorCopy(kd_nodes[i].mins, dl.mins);
		VectorCopy(kd_nodes[i].maxs, dl.maxs);
		fwrite(&dl, sizeof(dl), 1, f);
	}

	// Write surfaces
	vert_offset = 0;
	for (int i = 0; i < num_map_faces; i++)
	{
		cface_t *face = &map_faces[i];
		dkdsurface_t ds;

		// Find plane index
		ds.plane_num = 0;
		for (int j = 0; j < num_map_planes; j++)
		{
			if (&map_planes[j] == face->plane)
			{
				ds.plane_num = j;
				break;
			}
		}

		ds.flags = 0;
		ds.texture_num = FindOrCreateTexture(face->texture);
		ds.first_vertex = vert_offset;
		ds.num_vertices = face->num_verts;
		ds.lightmap_s = ds.lightmap_t = 0;
		ds.lightmap_w = ds.lightmap_h = 16;
		memcpy(ds.texinfo_vecs, face->texvecs, sizeof(ds.texinfo_vecs));

		fwrite(&ds, sizeof(ds), 1, f);
		vert_offset += face->num_verts;
	}

	// Write model
	dkdmodel_t dm;
	dm.mins[0] = dm.mins[1] = dm.mins[2] = -4096;
	dm.maxs[0] = dm.maxs[1] = dm.maxs[2] = 4096;
	dm.origin[0] = dm.origin[1] = dm.origin[2] = 0;
	dm.headnode = 0;
	dm.first_surface = 0;
	dm.num_surfaces = num_map_faces;

	// Calculate actual bounds
	for (int i = 0; i < num_map_faces; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			if (i == 0 || map_faces[i].mins[j] < dm.mins[j])
				dm.mins[j] = map_faces[i].mins[j];
			if (i == 0 || map_faces[i].maxs[j] > dm.maxs[j])
				dm.maxs[j] = map_faces[i].maxs[j];
		}
	}

	fwrite(&dm, sizeof(dm), 1, f);

	free(node_indices);
	free(entities);
	fclose(f);

	Con_Printf("Wrote %s (%d bytes)\n", fullpath, offset);
	Con_Printf("  %d planes, %d faces, %d verts\n", num_map_planes, num_map_faces, total_verts);
	Con_Printf("  %d nodes, %d leaves\n", node_count, leaf_count);
}

//=============================================================================
// Console Command
//=============================================================================

static void KD_Compile_f (void)
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("Usage: kd_compile <mapname.map> [output.qkd]\n");
		return;
	}

	const char *input = Cmd_Argv(1);
	char output[MAX_QPATH];

	if (Cmd_Argc() >= 3)
	{
		q_strlcpy(output, Cmd_Argv(2), sizeof(output));
	}
	else
	{
		// Replace .map with .qkd
		q_strlcpy(output, input, sizeof(output));
		char *ext = strrchr(output, '.');
		if (ext)
			strcpy(ext, ".qkd");
		else
			strcat(output, ".qkd");
	}

	Con_Printf("Compiling %s -> %s\n", input, output);

	// Clear compiler state
	ClearCompiler();

	// Parse map file
	if (!ParseMapFile(input))
	{
		Con_Printf("ERROR: Failed to parse map file\n");
		return;
	}

	// Convert brushes to faces
	Con_Printf("Converting brushes to faces...\n");
	for (int i = 0; i < num_map_brushes; i++)
	{
		int before = num_map_faces;
		BrushToFaces(&map_brushes[i]);
		Con_DPrintf("Brush %d: %d sides -> %d faces\n", i, map_brushes[i].num_sides, num_map_faces - before);
	}
	Con_Printf("Generated %d faces\n", num_map_faces);

	if (num_map_faces == 0)
	{
		Con_Printf("ERROR: No faces generated\n");
		return;
	}

	// Calculate world bounds
	vec3_t world_mins, world_maxs;
	VectorCopy(map_faces[0].mins, world_mins);
	VectorCopy(map_faces[0].maxs, world_maxs);

	for (int i = 1; i < num_map_faces; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			if (map_faces[i].mins[j] < world_mins[j])
				world_mins[j] = map_faces[i].mins[j];
			if (map_faces[i].maxs[j] > world_maxs[j])
				world_maxs[j] = map_faces[i].maxs[j];
		}
	}

	// Build face index array
	int *face_indices = (int *)malloc(num_map_faces * sizeof(int));
	for (int i = 0; i < num_map_faces; i++)
		face_indices[i] = i;

	// Build kd-tree
	Con_Printf("Building kd-tree...\n");
	kd_nodes = NULL;
	num_kd_nodes = 0;
	max_kd_nodes = 0;

	ckdnode_t *root = BuildKdTree(face_indices, num_map_faces, world_mins, world_maxs, 0);
	(void)root;

	free(face_indices);

	Con_Printf("Built %d nodes\n", num_kd_nodes);

	// Build Jolt collision world from faces
	Con_Printf("Building Jolt collision world...\n");

	// Count total vertices and triangles
	int total_verts = 0;
	int total_tris = 0;
	for (int i = 0; i < num_map_faces; i++)
	{
		if (map_faces[i].num_verts >= 3)
		{
			total_verts += map_faces[i].num_verts;
			// Fan triangulation: n vertices -> n-2 triangles
			total_tris += map_faces[i].num_verts - 2;
		}
	}

	Con_Printf("Jolt: %d vertices, %d triangles from %d faces\n", total_verts, total_tris, num_map_faces);

	if (total_verts > 0 && total_tris > 0)
	{
		// Allocate vertex and index arrays
		vec3_t *jolt_verts = (vec3_t *)malloc(total_verts * sizeof(vec3_t));
		int *jolt_indices = (int *)malloc(total_tris * 3 * sizeof(int));

		int vert_offset = 0;
		int index_offset = 0;

		for (int i = 0; i < num_map_faces; i++)
		{
			cface_t *face = &map_faces[i];
			if (face->num_verts < 3)
				continue;

			// Copy vertices
			int first_vert = vert_offset;
			for (int j = 0; j < face->num_verts; j++)
			{
				VectorCopy(face->verts[j], jolt_verts[vert_offset]);
				vert_offset++;
			}

			// Fan triangulate the polygon
			for (int j = 1; j < face->num_verts - 1; j++)
			{
				jolt_indices[index_offset++] = first_vert;
				jolt_indices[index_offset++] = first_vert + j;
				jolt_indices[index_offset++] = first_vert + j + 1;
			}
		}

		// Create Jolt world
		jolt_world_t *jolt_world = Jolt_CreateWorld(jolt_verts, total_verts, jolt_indices, total_tris * 3);
		if (jolt_world)
		{
			Jolt_SetActiveWorld(jolt_world);
			Con_Printf("Jolt collision world activated\n");
		}

		free(jolt_verts);
		free(jolt_indices);
	}

	// Also build SVO for collision (as fallback/comparison)
	Con_Printf("Building SVO for collision...\n");

	// Convert compiler brushes to mkdbrush_t format for SVO
	mkdbrush_t *svo_brushes = (mkdbrush_t *)calloc(num_map_brushes, sizeof(mkdbrush_t));
	for (int i = 0; i < num_map_brushes; i++)
	{
		cbrush_t *src = &map_brushes[i];
		mkdbrush_t *dst = &svo_brushes[i];

		dst->num_sides = src->num_sides;
		dst->sides = (mkdbrushside_t *)calloc(src->num_sides, sizeof(mkdbrushside_t));

		// Calculate brush bounds from plane points
		qboolean first_point = true;
		for (int j = 0; j < src->num_sides; j++)
		{
			cbrushside_t *src_side = &src->sides[j];
			mkdbrushside_t *dst_side = &dst->sides[j];

			// Allocate plane for this side
			dst_side->plane = (mplane_t *)calloc(1, sizeof(mplane_t));
			VectorCopy(src_side->orig_normal, dst_side->plane->normal);
			dst_side->plane->dist = src_side->orig_dist;

			// Update brush bounds from plane points
			for (int k = 0; k < 3; k++)
			{
				if (first_point)
				{
					VectorCopy(src_side->plane_points[k], dst->mins);
					VectorCopy(src_side->plane_points[k], dst->maxs);
					first_point = false;
				}
				else
				{
					for (int axis = 0; axis < 3; axis++)
					{
						if (src_side->plane_points[k][axis] < dst->mins[axis])
							dst->mins[axis] = src_side->plane_points[k][axis];
						if (src_side->plane_points[k][axis] > dst->maxs[axis])
							dst->maxs[axis] = src_side->plane_points[k][axis];
					}
				}
			}
		}
	}

	// Build the SVO
	svo_t *svo = SVO_Build(svo_brushes, num_map_brushes, world_mins, world_maxs);
	if (svo)
	{
		// Set as active collision tree
		SVO_SetCollisionTree(svo);
		Con_Printf("SVO collision tree activated\n");
	}

	// Free temporary brush data
	for (int i = 0; i < num_map_brushes; i++)
	{
		for (int j = 0; j < svo_brushes[i].num_sides; j++)
			free(svo_brushes[i].sides[j].plane);
		free(svo_brushes[i].sides);
	}
	free(svo_brushes);

	// Write output file
	WriteQKDFile(output);

	// Cleanup
	for (int i = 0; i < num_kd_nodes; i++)
	{
		if (kd_nodes[i].face_indices)
			free(kd_nodes[i].face_indices);
	}
	free(kd_nodes);
	kd_nodes = NULL;
}

//=============================================================================
// Initialization
//=============================================================================

void KD_CompileInit (void)
{
	Cmd_AddCommand("kd_compile", KD_Compile_f);
}
