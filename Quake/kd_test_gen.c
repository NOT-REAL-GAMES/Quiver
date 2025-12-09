/*
 * kd_test_gen.c - Test polygon clipping and generate .qkd files
 * Compile: g++ -o kd_test_gen kd_test_gen.c -lm
 * Run: ./kd_test_gen test       - test clipping algorithm
 *      ./kd_test_gen out.qkd    - generate test QKD file
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define KDFORMAT_MAGIC   (('K' << 0) | ('D' << 8) | ('M' << 16) | ('P' << 24))
#define KDFORMAT_VERSION 1

#define KD_AXIS_X    0
#define KD_AXIS_Y    1
#define KD_AXIS_Z    2
#define KD_AXIS_LEAF 255

#define POINT_EPSILON 0.1f
#define KDC_MAX_FACE_VERTS 32

typedef float vec3_t[3];

#define DotProduct(x,y) ((x)[0]*(y)[0]+(x)[1]*(y)[1]+(x)[2]*(y)[2])
#define VectorCopy(a,b) do { (b)[0]=(a)[0]; (b)[1]=(a)[1]; (b)[2]=(a)[2]; } while(0)
#define VectorScale(a,s,b) do { (b)[0]=(a)[0]*(s); (b)[1]=(a)[1]*(s); (b)[2]=(a)[2]*(s); } while(0)
#define VectorMA(a,s,b,c) do { (c)[0]=(a)[0]+(s)*(b)[0]; (c)[1]=(a)[1]+(s)*(b)[1]; (c)[2]=(a)[2]+(s)*(b)[2]; } while(0)
#define CrossProduct(a,b,c) do { (c)[0]=(a)[1]*(b)[2]-(a)[2]*(b)[1]; (c)[1]=(a)[2]*(b)[0]-(a)[0]*(b)[2]; (c)[2]=(a)[0]*(b)[1]-(a)[1]*(b)[0]; } while(0)

float VectorLength(vec3_t v)
{
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

// Clip a polygon by a plane
// keep_front: if true, keep front side (d > 0); if false, keep back side (d < 0)
static int ClipPolyByPlaneRaw (vec3_t *verts, int num_verts, vec3_t normal, float dist, vec3_t *out, int keep_front)
{
    float dists[KDC_MAX_FACE_VERTS + 1];
    int sides[KDC_MAX_FACE_VERTS + 1];
    int counts[3] = {0, 0, 0};
    int num_out = 0;

    int keep_side = keep_front ? 0 : 1;
    int clip_side = keep_front ? 1 : 0;

    if (num_verts > KDC_MAX_FACE_VERTS)
        num_verts = KDC_MAX_FACE_VERTS;

    // Classify vertices
    printf("  Classifying %d vertices:\n", num_verts);
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
        printf("    v%d: (%.2f, %.2f, %.2f) -> dist=%.2f side=%d (%s)\n",
            i, verts[i][0], verts[i][1], verts[i][2], d,
            sides[i], sides[i] == 0 ? "FRONT" : (sides[i] == 1 ? "BACK" : "ON"));
    }

    // Wrap around
    dists[num_verts] = dists[0];
    sides[num_verts] = sides[0];

    printf("  Counts: front=%d, back=%d, on=%d, keep_side=%d (%s)\n",
        counts[0], counts[1], counts[2], keep_side, keep_side == 0 ? "FRONT" : "BACK");

    // All on clip side = clipped away entirely
    if (!counts[keep_side] && !counts[2])
    {
        printf("  -> ALL ON CLIP SIDE, return 0\n");
        return 0;
    }

    // All on keep side or on plane = keep as is
    if (!counts[clip_side])
    {
        printf("  -> ALL ON KEEP SIDE, return %d as-is\n", num_verts);
        int copy_count = (num_verts > KDC_MAX_FACE_VERTS) ? KDC_MAX_FACE_VERTS : num_verts;
        for (int i = 0; i < copy_count; i++)
            VectorCopy(verts[i], out[i]);
        return copy_count;
    }

    printf("  Splitting polygon:\n");

    // Split the polygon
    for (int i = 0; i < num_verts; i++)
    {
        printf("    i=%d: sides[%d]=%d sides[%d]=%d | num_out=%d\n",
            i, i, sides[i], i+1, sides[i + 1], num_out);

        if (sides[i] == keep_side || sides[i] == 2)
        {
            if (num_out >= KDC_MAX_FACE_VERTS - 1)
            {
                printf("      -> OVERFLOW\n");
                break;
            }
            VectorCopy(verts[i], out[num_out]);
            printf("      -> KEPT v%d at out[%d]\n", i, num_out);
            num_out++;
        }

        if (sides[i] == 2 || sides[i + 1] == 2)
        {
            printf("      -> skip split (on-plane)\n");
            continue;
        }

        if (sides[i] != sides[i + 1])
        {
            if (num_out >= KDC_MAX_FACE_VERTS - 1)
            {
                printf("      -> OVERFLOW\n");
                break;
            }
            float frac = dists[i] / (dists[i] - dists[i + 1]);
            vec3_t *p1 = &verts[i];
            vec3_t *p2 = &verts[(i + 1) % num_verts];

            for (int j = 0; j < 3; j++)
                out[num_out][j] = (*p1)[j] + frac * ((*p2)[j] - (*p1)[j]);
            printf("      -> SPLIT at out[%d] frac=%.4f pos=(%.2f, %.2f, %.2f)\n",
                num_out, frac, out[num_out][0], out[num_out][1], out[num_out][2]);
            num_out++;
        }
        printf("      -> after i=%d: num_out=%d\n", i, num_out);
    }

    printf("  FINAL OUTPUT: %d vertices\n", num_out);
    return num_out;
}

static int CreateBaseWindingRaw (vec3_t normal, float dist, vec3_t *verts)
{
    int axis = 0;
    float maxv = fabsf(normal[0]);
    if (fabsf(normal[1]) > maxv) { maxv = fabsf(normal[1]); axis = 1; }
    if (fabsf(normal[2]) > maxv) { axis = 2; }

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

    vec3_t center;
    VectorScale(normal, dist, center);

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

void test_simple_clip()
{
    printf("=== Testing Simple Polygon Clipping ===\n\n");

    // Create a simple square on the Z=0 plane from (-100,-100) to (100,100)
    vec3_t verts[KDC_MAX_FACE_VERTS] = {
        {-100, -100, 0},
        { 100, -100, 0},
        { 100,  100, 0},
        {-100,  100, 0}
    };
    vec3_t out[KDC_MAX_FACE_VERTS];

    printf("Input polygon: 4 vertices at Z=0\n");
    for (int i = 0; i < 4; i++)
        printf("  v%d: (%.2f, %.2f, %.2f)\n", i, verts[i][0], verts[i][1], verts[i][2]);

    // Clip by plane X=50 (keep X < 50, which is the back side)
    printf("\n--- Clip by plane X=50, keep_front=false (keep X < 50) ---\n");
    vec3_t clip_normal = {1, 0, 0};
    float clip_dist = 50;

    int num_out = ClipPolyByPlaneRaw(verts, 4, clip_normal, clip_dist, out, 0);

    printf("\nResult: %d vertices\n", num_out);
    for (int i = 0; i < num_out; i++)
        printf("  out[%d]: (%.2f, %.2f, %.2f)\n", i, out[i][0], out[i][1], out[i][2]);

    // Expected: 4 vertices: (-100,-100), (50,-100), (50,100), (-100,100)
    printf("\nExpected: 4 vertices forming rectangle from X=-100 to X=50\n");
}

void test_box_brush()
{
    printf("\n=== Testing Box Brush Face Generation ===\n\n");

    // A simple cube from (-64,-64,-64) to (64,64,64)
    // 6 planes, normals pointing outward from brush solid
    vec3_t normals[6] = {
        {0, 0, 1},   // top (Z=64)
        {0, 0, -1},  // bottom (Z=-64)
        {1, 0, 0},   // right (X=64)
        {-1, 0, 0},  // left (X=-64)
        {0, 1, 0},   // front (Y=64)
        {0, -1, 0}   // back (Y=-64)
    };
    float dists[6] = {64, 64, 64, 64, 64, 64};

    printf("Brush: cube from (-64,-64,-64) to (64,64,64)\n\n");

    // Generate face for side 0 (top, Z=64)
    int side_idx = 0;
    printf("=== Generating face for side %d: normal=(%.1f,%.1f,%.1f) dist=%.1f ===\n",
        side_idx, normals[side_idx][0], normals[side_idx][1], normals[side_idx][2], dists[side_idx]);

    vec3_t temp_verts[KDC_MAX_FACE_VERTS];
    vec3_t clipped_verts[KDC_MAX_FACE_VERTS];

    int num_verts = CreateBaseWindingRaw(normals[side_idx], dists[side_idx], temp_verts);
    printf("\nCreated base winding with %d vertices:\n", num_verts);
    for (int i = 0; i < num_verts; i++)
        printf("  v%d: (%.2f, %.2f, %.2f)\n", i, temp_verts[i][0], temp_verts[i][1], temp_verts[i][2]);

    // Clip by all other planes
    for (int clip_idx = 0; clip_idx < 6; clip_idx++)
    {
        if (clip_idx == side_idx) continue;

        printf("\n--- Clipping by side %d: normal=(%.1f,%.1f,%.1f) dist=%.1f, keep_front=FALSE (keep inside) ---\n",
            clip_idx, normals[clip_idx][0], normals[clip_idx][1], normals[clip_idx][2], dists[clip_idx]);

        // Keep vertices BEHIND the plane (inside the brush) - keep_front=false
        num_verts = ClipPolyByPlaneRaw(temp_verts, num_verts, normals[clip_idx], dists[clip_idx], clipped_verts, 0);

        if (num_verts < 3)
        {
            printf("CLIPPED AWAY!\n");
            break;
        }

        // Copy back
        for (int k = 0; k < num_verts; k++)
            VectorCopy(clipped_verts[k], temp_verts[k]);

        printf("After clip: %d vertices\n", num_verts);
        for (int i = 0; i < num_verts; i++)
            printf("  v%d: (%.2f, %.2f, %.2f)\n", i, temp_verts[i][0], temp_verts[i][1], temp_verts[i][2]);
    }

    printf("\n=== Final result for side %d: %d vertices ===\n", side_idx, num_verts);
    for (int i = 0; i < num_verts; i++)
        printf("  v%d: (%.2f, %.2f, %.2f)\n", i, temp_verts[i][0], temp_verts[i][1], temp_verts[i][2]);

    printf("\nExpected: 4 vertices forming the top face at Z=64\n");
    printf("  (-64,-64,64), (64,-64,64), (64,64,64), (-64,64,64)\n");
}

// Lump IDs for QKD generation
enum {
	KD_LUMP_ENTITIES,
	KD_LUMP_TEXTURES,
	KD_LUMP_PLANES,
	KD_LUMP_VERTICES,
	KD_LUMP_TEXCOORDS,
	KD_LUMP_KDNODES,
	KD_LUMP_KDLEAVES,
	KD_LUMP_SURFACES,
	KD_LUMP_SURFEDGES,
	KD_LUMP_VISIBILITY,
	KD_LUMP_LIGHTMAPS,
	KD_LUMP_BRUSHES,
	KD_LUMP_BRUSHSIDES,
	KD_LUMP_ZONES,
	KD_LUMP_MODELS,
	KD_LUMP_NUM
};

typedef struct {
	int32_t offset;
	int32_t length;
} dkdlump_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	dkdlump_t lumps[KD_LUMP_NUM];
} dkdheader_t;

typedef struct {
	uint8_t axis;
	uint8_t pad[3];
	float split_dist;
	int32_t children[2];
	float mins[3];
	float maxs[3];
} dkdnode_t;

typedef struct {
	int32_t contents;
	int32_t zone;
	int32_t first_surface;
	int32_t num_surfaces;
	float mins[3];
	float maxs[3];
} dkdleaf_t;

typedef struct {
	float position[3];
} dkdvertex_t;

typedef struct {
	float normal[3];
	float dist;
	int32_t type;
} dkdplane_t;

typedef struct {
	int32_t plane_num;
	uint32_t flags;
	int32_t texture_num;
	int32_t first_vertex;
	int32_t num_vertices;
	int32_t lightmap_s;
	int32_t lightmap_t;
	int32_t lightmap_w;
	int32_t lightmap_h;
	float texinfo_vecs[2][4];
} dkdsurface_t;

typedef struct {
	float mins[3];
	float maxs[3];
	float origin[3];
	int32_t headnode;
	int32_t first_surface;
	int32_t num_surfaces;
} dkdmodel_t;

#define CONTENTS_EMPTY -1
#define CONTENTS_SOLID -2
#define ROOM_SIZE_X 256.0f
#define ROOM_SIZE_Y 256.0f
#define ROOM_SIZE_Z 128.0f

int generate_qkd(const char *filename)
{
	FILE *f = fopen(filename, "wb");
	if (!f) {
		printf("Error: couldn't create %s\n", filename);
		return 1;
	}

	const char *entities = "{\n\"classname\" \"worldspawn\"\n}\n";
	int entities_len = strlen(entities);

	float hx = ROOM_SIZE_X / 2;
	float hy = ROOM_SIZE_Y / 2;
	float hz = ROOM_SIZE_Z / 2;

	dkdplane_t planes[6] = {
		{{ 1, 0, 0}, -hx, 0},
		{{-1, 0, 0}, -hx, 0},
		{{ 0, 1, 0}, -hy, 1},
		{{ 0,-1, 0}, -hy, 1},
		{{ 0, 0, 1}, -hz, 2},
		{{ 0, 0,-1}, -hz, 2},
	};

	dkdvertex_t face_verts[24] = {
		{{-hx, -hy, -hz}}, {{-hx,  hy, -hz}}, {{ hx,  hy, -hz}}, {{ hx, -hy, -hz}},
		{{-hx, -hy,  hz}}, {{ hx, -hy,  hz}}, {{ hx,  hy,  hz}}, {{-hx,  hy,  hz}},
		{{-hx, -hy, -hz}}, {{-hx, -hy,  hz}}, {{-hx,  hy,  hz}}, {{-hx,  hy, -hz}},
		{{ hx,  hy, -hz}}, {{ hx,  hy,  hz}}, {{ hx, -hy,  hz}}, {{ hx, -hy, -hz}},
		{{ hx, -hy, -hz}}, {{ hx, -hy,  hz}}, {{-hx, -hy,  hz}}, {{-hx, -hy, -hz}},
		{{-hx,  hy, -hz}}, {{-hx,  hy,  hz}}, {{ hx,  hy,  hz}}, {{ hx,  hy, -hz}},
	};

	dkdsurface_t surfaces[6] = {
		{5, 0, 0, 0, 4, 0, 0, 16, 16, {{0.0625f, 0, 0, 0}, {0, 0.0625f, 0, 0}}},
		{4, 0, 0, 4, 4, 0, 0, 16, 16, {{0.0625f, 0, 0, 0}, {0, 0.0625f, 0, 0}}},
		{1, 0, 0, 8, 4, 0, 0, 16, 16, {{0, 0.0625f, 0, 0}, {0, 0, 0.0625f, 0}}},
		{0, 0, 0, 12, 4, 0, 0, 16, 16, {{0, 0.0625f, 0, 0}, {0, 0, 0.0625f, 0}}},
		{3, 0, 0, 16, 4, 0, 0, 16, 16, {{0.0625f, 0, 0, 0}, {0, 0, 0.0625f, 0}}},
		{2, 0, 0, 20, 4, 0, 0, 16, 16, {{0.0625f, 0, 0, 0}, {0, 0, 0.0625f, 0}}},
	};

	dkdleaf_t leaves[7] = {
		{CONTENTS_SOLID, 0, 0, 0, {-1e6f, -1e6f, -1e6f}, {-hx, 1e6f, 1e6f}},
		{CONTENTS_SOLID, 0, 0, 0, {hx, -1e6f, -1e6f}, {1e6f, 1e6f, 1e6f}},
		{CONTENTS_SOLID, 0, 0, 0, {-hx, -1e6f, -1e6f}, {hx, -hy, 1e6f}},
		{CONTENTS_SOLID, 0, 0, 0, {-hx, hy, -1e6f}, {hx, 1e6f, 1e6f}},
		{CONTENTS_SOLID, 0, 0, 0, {-hx, -hy, -1e6f}, {hx, hy, -hz}},
		{CONTENTS_SOLID, 0, 0, 0, {-hx, -hy, hz}, {hx, hy, 1e6f}},
		{CONTENTS_EMPTY, 0, 0, 6, {-hx, -hy, -hz}, {hx, hy, hz}},
	};

	dkdnode_t nodes[6] = {
		{KD_AXIS_X, {0,0,0}, -hx, {1, ~0}, {-1e6f, -1e6f, -1e6f}, {1e6f, 1e6f, 1e6f}},
		{KD_AXIS_X, {0,0,0}, hx, {~1, 2}, {-hx, -1e6f, -1e6f}, {1e6f, 1e6f, 1e6f}},
		{KD_AXIS_Y, {0,0,0}, -hy, {3, ~2}, {-hx, -1e6f, -1e6f}, {hx, 1e6f, 1e6f}},
		{KD_AXIS_Y, {0,0,0}, hy, {~3, 4}, {-hx, -hy, -1e6f}, {hx, 1e6f, 1e6f}},
		{KD_AXIS_Z, {0,0,0}, -hz, {5, ~4}, {-hx, -hy, -1e6f}, {hx, hy, 1e6f}},
		{KD_AXIS_Z, {0,0,0}, hz, {~5, ~6}, {-hx, -hy, -hz}, {hx, hy, 1e6f}},
	};

	dkdmodel_t models[1] = {
		{{-hx, -hy, -hz}, {hx, hy, hz}, {0, 0, 0}, 0, 0, 6}
	};

	dkdheader_t header;
	memset(&header, 0, sizeof(header));
	header.magic = KDFORMAT_MAGIC;
	header.version = KDFORMAT_VERSION;

	int offset = sizeof(dkdheader_t);

	header.lumps[KD_LUMP_ENTITIES].offset = offset;
	header.lumps[KD_LUMP_ENTITIES].length = entities_len;
	offset += entities_len;

	header.lumps[KD_LUMP_PLANES].offset = offset;
	header.lumps[KD_LUMP_PLANES].length = sizeof(planes);
	offset += sizeof(planes);

	header.lumps[KD_LUMP_VERTICES].offset = offset;
	header.lumps[KD_LUMP_VERTICES].length = sizeof(face_verts);
	offset += sizeof(face_verts);

	header.lumps[KD_LUMP_KDNODES].offset = offset;
	header.lumps[KD_LUMP_KDNODES].length = sizeof(nodes);
	offset += sizeof(nodes);

	header.lumps[KD_LUMP_KDLEAVES].offset = offset;
	header.lumps[KD_LUMP_KDLEAVES].length = sizeof(leaves);
	offset += sizeof(leaves);

	header.lumps[KD_LUMP_SURFACES].offset = offset;
	header.lumps[KD_LUMP_SURFACES].length = sizeof(surfaces);
	offset += sizeof(surfaces);

	header.lumps[KD_LUMP_MODELS].offset = offset;
	header.lumps[KD_LUMP_MODELS].length = sizeof(models);
	offset += sizeof(models);

	fwrite(&header, sizeof(header), 1, f);
	fwrite(entities, entities_len, 1, f);
	fwrite(planes, sizeof(planes), 1, f);
	fwrite(face_verts, sizeof(face_verts), 1, f);
	fwrite(nodes, sizeof(nodes), 1, f);
	fwrite(leaves, sizeof(leaves), 1, f);
	fwrite(surfaces, sizeof(surfaces), 1, f);
	fwrite(models, sizeof(models), 1, f);

	fclose(f);

	printf("Created %s (%d bytes)\n", filename, offset);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: %s test         - test clipping algorithm\n", argv[0]);
		printf("       %s <output.qkd> - generate test QKD file\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "test") == 0) {
		test_simple_clip();
		test_box_brush();
		return 0;
	}

	return generate_qkd(argv[1]);
}
