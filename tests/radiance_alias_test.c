#include "../Quake/radiance_alias.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void close_enough (float a, float b) { assert (fabsf(a-b)<1e-4f); }

/* Independent declarations of the GPU streams: these offsets are part of the
 * loader/shader contract, rather than a test-only float-position format. */
typedef struct
{
	float xyz[3], normal[3], uv[2];
	unsigned char weights[4], joints[4];
	float local[3][4];
} skin4_t;
typedef struct
{
	float xyz[3], normal[3], uv[2];
	unsigned char weights[8], joints[8];
	float local[3][8];
} skin8_t;
_Static_assert (sizeof (skin4_t) == 88 && offsetof (skin4_t, local) == 40, "four-weight GPU layout");
_Static_assert (sizeof (skin8_t) == 144 && offsetof (skin8_t, local) == 48, "eight-weight GPU layout");
static const float identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
static const float joint_identity[12] = {1,0,0,0,0,1,0,0,0,0,1,0};
static const uint16_t triangle_indices[3] = {0,1,2};

static void test_static_identity (void)
{
	skin4_t vertices[3] = {0};
	float joints[12]; memcpy (joints, joint_identity, sizeof (joints));
	uint16_t indices[3]; memcpy (indices, triangle_indices, sizeof (indices));
	for (int v = 0; v < 3; ++v)
	{
		/* The bind-position prefix must not be used, even when it is NaN.
		 * Keep a large fractional coordinate that cannot fit MDL/MD3 packing. */
		vertices[v].xyz[0] = NAN;
		vertices[v].weights[0] = 255;
		vertices[v].local[0][0] = 100000.25f + (v == 1);
		vertices[v].local[1][0] = v == 2;
		vertices[v].uv[0] = v == 1; vertices[v].uv[1] = v == 2;
	}
	rc_alias_mesh_t *mesh = RCAlias_CreateSkinnedStatic (3, 3, vertices, sizeof (*vertices), 4, 1, joints, indices);
	assert (mesh && RCAlias_TotalBytes () == RCAlias_Bytes (mesh));
	/* All data needed after model loading belongs to the retained allocation. */
	memset (vertices, 0xff, sizeof (vertices)); memset (joints, 0xff, sizeof (joints)); memset (indices, 0xff, sizeof (indices));
	rc_scene_t scene; RCScene_Init (&scene);
	float matrix[16]; memcpy (matrix, identity, sizeof (matrix));
	matrix[12] = 10; matrix[13] = 20; matrix[14] = 30;
	assert (RCAlias_Append (&scene, mesh, 0, 0, .75f, matrix, 7, RC_SCENE_CUTOUT, 11, 22));
	assert (scene.triangle_count == 1 && RCScene_Build (&scene));
	float origin[3] = {100010.5f,20.25f,35}, direction[3] = {0,0,-1}; rc_scene_hit_t hit;
	assert (RCScene_TraceReference (&scene, origin, direction, 0, 10, NULL, NULL, &hit));
	close_enough (hit.distance, 5); close_enough (hit.uv[0], .25f); close_enough (hit.uv[1], .25f);
	assert (hit.material == 7 && hit.surface == 11 && hit.instance == 22 && (hit.flags & RC_SCENE_CUTOUT));
	assert (!RCAlias_Append (&scene, mesh, 0, 1, 0, identity, 0, 0, 0, 0));
	assert (scene.triangle_count == 1);
	RCAlias_Destroy (mesh); RCScene_Clear (&scene); assert (!RCAlias_TotalBytes ());
}

/* Double-precision oracle deliberately uses fields, byte/sum normalization,
 * and explicit rows. Joint-local xyz are already weighted: multiplying them
 * by the quantized byte a second time is an incorrect bind-pose shortcut. */
static void reference_skin (unsigned count, const unsigned char *weights, const unsigned char *indices,
	const float *local, const float *joints, const float matrix[16], double out[3])
{
	unsigned sum = 0; for (unsigned j = 0; j < count; ++j) sum += weights[j];
	double position[3] = {0};
	for (unsigned j = 0; j < count; ++j)
	{
		const float *row = joints + indices[j] * 12;
		double w = sum ? (double)weights[j] / sum : 0;
		for (unsigned a = 0; a < 3; ++a)
			position[a] += row[a*4] * (double)local[j] + row[a*4+1] * (double)local[count+j] +
				row[a*4+2] * (double)local[count*2+j] + row[a*4+3] * w;
	}
	for (unsigned a = 0; a < 3; ++a)
		out[a] = matrix[a] * position[0] + matrix[4+a] * position[1] + matrix[8+a] * position[2] + matrix[12+a];
}

static void test_static_multiple_weights (void)
{
	/* Joint rotations and nonzero translations expose transposed-matrix,
	 * unnormalized-byte, dropped-zero-weight, and missing-second-group errors. */
	float joints[8][12];
	for (int j = 0; j < 8; ++j)
	{
		memcpy (joints[j], joint_identity, sizeof (joint_identity));
		if (j & 1) { joints[j][0] = 0; joints[j][1] = -1; joints[j][4] = 1; joints[j][5] = 0; }
		if (j & 2) { joints[j][5] *= -1; joints[j][6] = .5f; joints[j][10] = .75f; }
		joints[j][3] = j*3-4; joints[j][7] = 7-j; joints[j][11] = j*.5f;
	}
	const float matrix[16] = {0,2,0,0,-3,0,0,0,0,0,.5f,0,10,-20,30,1};
	for (unsigned count = 4; count <= 8; count += 4)
	{
		/* Padded eight-weight vertices also exercise the explicit stride. */
		struct { skin8_t vertex; unsigned char padding[8]; } eight[3] = {0};
		skin4_t four[3] = {0};
		double expected[3][3];
		for (int v = 0; v < 3; ++v)
		{
			unsigned char weights[8] = {100,80,74,0,0,0,0,0};
			if (count == 8) { const unsigned char all[8] = {1,2,3,4,31,63,64,84}; memcpy (weights, all, 8); }
			unsigned char indices[8]; float local[24];
			for (unsigned j = 0; j < count; ++j)
			{
				indices[j] = j;
				for (unsigned a = 0; a < 3; ++a)
					local[a*count+j] = (j+1)*.125f + v*(a+1)*.25f;
			}
			if (count == 4)
			{
				memcpy (four[v].weights, weights, 4); memcpy (four[v].joints, indices, 4);
				memcpy (four[v].local, local, sizeof (four[v].local)); four[v].xyz[0] = 5000;
			}
			else
			{
				memcpy (eight[v].vertex.weights, weights, 8); memcpy (eight[v].vertex.joints, indices, 8);
				memcpy (eight[v].vertex.local, local, sizeof (eight[v].vertex.local)); eight[v].vertex.xyz[0] = 5000;
			}
			reference_skin (count, weights, indices, local, &joints[0][0], matrix, expected[v]);
		}
		rc_alias_mesh_t *mesh = RCAlias_CreateSkinnedStatic (3, 3, count == 4 ? (void *)four : (void *)eight,
			count == 4 ? sizeof (*four) : sizeof (*eight), count, 8, &joints[0][0], triangle_indices);
		assert (mesh);
		rc_scene_t scene; RCScene_Init (&scene);
		assert (RCAlias_Append (&scene, mesh, 0, 0, 0, matrix, 0, 0, 0, 0));
		for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a)
			assert (fabs (scene.triangles[0].position[v][a] - expected[v][a]) < 1e-4);
		RCAlias_Destroy (mesh); RCScene_Clear (&scene); assert (!RCAlias_TotalBytes ());
	}
	/* An all-zero packed group still rotates its preweighted local positions;
	 * only the translation contribution is zero. */
	skin4_t zero[3] = {0};
	zero[0].local[0][0] = 1; zero[1].local[1][0] = 1; zero[2].local[2][0] = 1;
	rc_alias_mesh_t *mesh = RCAlias_CreateSkinnedStatic (3, 3, zero, sizeof (*zero), 4, 8, &joints[0][0], triangle_indices);
	rc_scene_t scene; RCScene_Init (&scene); assert (mesh && RCAlias_Append (&scene, mesh, 0, 0, 0, identity, 0, 0, 0, 0));
	close_enough (scene.triangles[0].position[0][0], 1);
	close_enough (scene.triangles[0].position[0][1], 0);
	RCAlias_Destroy (mesh); RCScene_Clear (&scene); assert (!RCAlias_TotalBytes ());
}

static void test_static_rejection_and_limits (void)
{
	skin4_t valid[3] = {0}, vertices[3];
	for (int i = 0; i < 3; ++i) valid[i].weights[0] = 255;
	valid[1].local[0][0] = 1; valid[2].local[1][0] = 1;
	float joints[12]; memcpy (joints, joint_identity, sizeof (joints));
	uint16_t indices[3]; memcpy (indices, triangle_indices, sizeof (indices));
#define REJECT(vertices_, stride_, influences_, joints_, rows_, indices_) \
	do { assert (!RCAlias_CreateSkinnedStatic (3, 3, vertices_, stride_, influences_, joints_, rows_, indices_)); \
	assert (!RCAlias_TotalBytes ()); } while (0)
	REJECT (valid, sizeof (*valid), 3, 1, joints, indices);
	REJECT (valid, sizeof (*valid)-1, 4, 1, joints, indices);
	REJECT (valid, SIZE_MAX, 4, 1, joints, indices);
	REJECT (valid, sizeof (*valid), 4, 0, joints, indices);
	REJECT (valid, sizeof (*valid), 4, 65537, joints, indices);
	indices[2] = 3; REJECT (valid, sizeof (*valid), 4, 1, joints, indices); indices[2] = 2;
	memcpy (vertices, valid, sizeof (vertices)); vertices[0].joints[3] = 1; /* weight zero still loads this matrix */
	REJECT (vertices, sizeof (*vertices), 4, 1, joints, indices);
	memcpy (vertices, valid, sizeof (vertices)); vertices[0].local[2][3] = NAN;
	REJECT (vertices, sizeof (*vertices), 4, 1, joints, indices);
	memcpy (vertices, valid, sizeof (vertices)); vertices[2].uv[1] = INFINITY;
	REJECT (vertices, sizeof (*vertices), 4, 1, joints, indices);
	joints[11] = NAN; REJECT (valid, sizeof (*valid), 4, 1, joints, indices);
	memcpy (joints, joint_identity, sizeof (joints)); joints[0] = FLT_MAX;
	memcpy (vertices, valid, sizeof (vertices)); vertices[1].local[0][0] = 2;
	REJECT (vertices, sizeof (*vertices), 4, 1, joints, indices);
	memcpy (joints, joint_identity, sizeof (joints));
#undef REJECT
	/* A partially appended mesh must roll back if a later triangle overflows
	 * the world transform. One triangle first succeeds to exercise rollback. */
	skin4_t two[4] = {0};
	for (int i = 0; i < 4; ++i) two[i].weights[0] = 255;
	two[1].local[0][0] = 1; two[2].local[1][0] = 1; two[3].local[0][0] = 2;
	uint16_t two_indices[6] = {0,1,2,0,3,2};
	rc_alias_mesh_t *mesh = RCAlias_CreateSkinnedStatic (4, 6, two, sizeof (*two), 4, 1, joints, two_indices);
	rc_scene_t scene; RCScene_Init (&scene); float matrix[16]; memcpy (matrix, identity, sizeof (matrix)); matrix[0] = FLT_MAX;
	assert (mesh && !RCAlias_Append (&scene, mesh, 0, 0, 0, matrix, 0, 0, 0, 0));
	assert (!scene.triangle_count);
	RCAlias_Destroy (mesh); RCScene_Clear (&scene); assert (!RCAlias_TotalBytes ());
	/* The uint16 limit is per surface: index65535 remains representable. */
	skin4_t *large = calloc (65536, sizeof (*large)); assert (large);
	for (unsigned i = 0; i < 65536; ++i) large[i].weights[0] = 255;
	large[65534].local[0][0] = 1; large[65535].local[1][0] = 1;
	uint16_t last[3] = {65533,65534,65535};
	mesh = RCAlias_CreateSkinnedStatic (65536, 3, large, sizeof (*large), 4, 1, joints, last);
	assert (mesh && RCAlias_Append (&scene, mesh, 0, 0, 0, identity, 0, 0, 0, 0));
	close_enough (scene.triangles[0].position[1][0], 1); close_enough (scene.triangles[0].position[2][1], 1);
	assert (!RCAlias_CreateSkinnedStatic (65537, 3, large, sizeof (*large), 4, 1, joints, last));
	RCAlias_Destroy (mesh); RCScene_Clear (&scene); free (large); assert (!RCAlias_TotalBytes ());
}

int main (void)
{
	assert (RCAlias_TotalBytes () == 0);
	/* Packed XYZ has the same 4x16-bit stride as meshxyz_t, with an extra
	 * ignored normal tail; retain must own the source after it disappears. */
	struct vertex { uint16_t xyz[4]; unsigned char normal[4]; } positions[2][3] = {0};
	positions[0][1].xyz[0]=257; positions[0][2].xyz[1]=257;
	for (int i=0;i<3;++i) { positions[1][i]=positions[0][i]; positions[1][i].xyz[2]=514; }
	float uv[3][2]={{0,0},{1,0},{0,1}}; uint16_t indices[3]={0,1,2};
	float scale[3]={2,3,4}, offset[3]={5,6,7};
	rc_alias_mesh_t *mdl=RCAlias_Create (RC_ALIAS_MDL,3,2,3,positions,sizeof (struct vertex),uv,sizeof (uv[0]),indices,scale,offset);
	assert (mdl && RCAlias_Bytes (mdl) > sizeof (positions));
	assert (RCAlias_TotalBytes () == RCAlias_Bytes (mdl));
	memset (positions,0,sizeof (positions)); memset (uv,0,sizeof (uv)); memset (indices,0,sizeof (indices));
	float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
	rc_scene_t scene; RCScene_Init (&scene);
	assert (RCAlias_Append (&scene,mdl,0,1,.5f,matrix,7,RC_SCENE_CUTOUT,11,22));
	assert (scene.triangle_count==1 && RCScene_Build (&scene));
	float origin[3]={15.5f,26.75f,50}, direction[3]={0,0,-1}; rc_scene_hit_t hit;
	assert (RCScene_TraceReference (&scene,origin,direction,0,20,NULL,NULL,&hit));
	close_enough (hit.distance,9); close_enough (hit.uv[0],.25f); close_enough (hit.uv[1],.25f);
	assert (hit.material==7 && hit.surface==11 && hit.instance==22 && (hit.flags & RC_SCENE_CUTOUT));
	assert (!RCAlias_Append (&scene,mdl,0,2,.5f,matrix,7,0,0,0) && scene.triangle_count==1);
	RCAlias_Destroy (mdl); RCScene_Clear (&scene);
	assert (RCAlias_TotalBytes () == 0);
	/* MD3 values are decoded from biased signed 16-bit before model scaling. */
	memset (positions,0,sizeof (positions));
	for (int p=0;p<2;++p) for (int v=0;v<3;++v) for (int a=0;a<3;++a) positions[p][v].xyz[a]=32768;
	for (int p=0;p<2;++p) { positions[p][1].xyz[0]+=64; positions[p][2].xyz[1]+=64; }
	for (int v=0;v<3;++v) positions[1][v].xyz[2]+=64;
	indices[0]=0; indices[1]=1; indices[2]=2; uv[1][0]=1; uv[2][1]=1;
	float md3scale[3]={1.f/64,1.f/64,1.f/64}, zero[3]={0,0,0};
	rc_alias_mesh_t *md3=RCAlias_Create (RC_ALIAS_MD3,3,2,3,positions,sizeof (struct vertex),uv,sizeof (uv[0]),indices,md3scale,zero);
	assert (md3 && RCAlias_Append (&scene,md3,0,1,.5f,matrix,8,0,0,9) && RCScene_Build (&scene));
	float md3origin[3]={10.25f,20.25f,32};
	assert (RCScene_TraceReference (&scene,md3origin,direction,0,10,NULL,NULL,&hit));
	close_enough (hit.distance,1.5f); close_enough (hit.uv[0],.25f); close_enough (hit.uv[1],.25f);
	indices[2]=3;
	assert (!RCAlias_Create (RC_ALIAS_MD3,3,2,3,positions,sizeof (struct vertex),uv,sizeof (uv[0]),indices,md3scale,zero));
	RCAlias_Destroy (md3); RCScene_Clear (&scene);
	assert (RCAlias_TotalBytes () == 0);
	test_static_identity ();
	test_static_multiple_weights ();
	test_static_rejection_and_limits ();
	puts ("PASS: retained MDL/MD3 poses and static MD5 four/eight-weight skinning, ownership, limits and rollback");
	return 0;
}
