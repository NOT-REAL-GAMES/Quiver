#include "atomics.h"
#include "radiance_alias.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct rc_alias_mesh_s
{
	rc_alias_format_t format;
	uint32_t num_vertices, num_poses, num_indices;
	uint16_t *xyz, *indices;
	float *uv, *float_xyz;
	float scale[3], scale_origin[3];
	size_t bytes;
	uint64_t revision;
};

static atomic_uint64_t rc_alias_total_bytes, rc_alias_next_revision;

rc_alias_mesh_t *RCAlias_Create (rc_alias_format_t format, uint32_t num_vertices, uint32_t num_poses,
	uint32_t num_indices, const void *xyz, size_t xyz_stride, const void *uv, size_t uv_stride,
	const uint16_t *indices, const float scale[3], const float scale_origin[3])
{
	if ((format != RC_ALIAS_MDL && format != RC_ALIAS_MD3) || !num_vertices || num_vertices > 65536u || !num_poses ||
		!num_indices || num_indices % 3 || !xyz || !uv || !indices || !scale || !scale_origin ||
		xyz_stride < 3 * sizeof (uint16_t) || uv_stride < 2 * sizeof (float)) return NULL;
	uint64_t count = (uint64_t)num_vertices * num_poses;
	if (count > SIZE_MAX / (3 * sizeof (uint16_t)) || count > SIZE_MAX / xyz_stride ||
		(uint64_t)num_vertices > SIZE_MAX / uv_stride || (uint64_t)num_vertices * 2 * sizeof (float) > SIZE_MAX ||
		(uint64_t)num_indices * sizeof (uint16_t) > SIZE_MAX) return NULL;
	for (int a = 0; a < 3; ++a) if (!isfinite (scale[a]) || !isfinite (scale_origin[a])) return NULL;
	for (uint32_t i = 0; i < num_indices; ++i) if (indices[i] >= num_vertices) return NULL;
	rc_alias_mesh_t *mesh = calloc (1, sizeof (*mesh));
	if (!mesh) return NULL;
	size_t xyz_bytes = (size_t)count * 3 * sizeof (*mesh->xyz), uv_bytes = (size_t)num_vertices * 2 * sizeof (*mesh->uv),
		index_bytes = (size_t)num_indices * sizeof (*mesh->indices);
	if (xyz_bytes > SIZE_MAX - uv_bytes || xyz_bytes + uv_bytes > SIZE_MAX - index_bytes ||
		xyz_bytes + uv_bytes + index_bytes > SIZE_MAX - sizeof (*mesh)) goto failed;
	mesh->xyz = malloc (xyz_bytes); mesh->uv = malloc (uv_bytes); mesh->indices = malloc (index_bytes);
	if (!mesh->xyz || !mesh->uv || !mesh->indices) goto failed;
	for (size_t i = 0; i < count; ++i)
		memcpy (mesh->xyz + i * 3, (const unsigned char *)xyz + i * xyz_stride, 3 * sizeof (uint16_t));
	for (uint32_t i = 0; i < num_vertices; ++i)
	{
		memcpy (mesh->uv + i * 2, (const unsigned char *)uv + (size_t)i * uv_stride, 2 * sizeof (float));
		if (!isfinite (mesh->uv[i * 2]) || !isfinite (mesh->uv[i * 2 + 1])) goto failed;
	}
	memcpy (mesh->indices, indices, index_bytes);
	memcpy (mesh->scale, scale, sizeof (mesh->scale));
	memcpy (mesh->scale_origin, scale_origin, sizeof (mesh->scale_origin));
	mesh->format = format; mesh->num_vertices = num_vertices; mesh->num_poses = num_poses; mesh->num_indices = num_indices;
	mesh->bytes = sizeof (*mesh) + xyz_bytes + uv_bytes + index_bytes;
	mesh->revision = Atomic_AddUInt64 (&rc_alias_next_revision, 1) + 1;
	if (!mesh->revision) mesh->revision = Atomic_AddUInt64 (&rc_alias_next_revision, 1) + 1;
	Atomic_AddUInt64 (&rc_alias_total_bytes, (uint64_t)mesh->bytes);
	return mesh;
failed:
	RCAlias_Destroy (mesh);
	return NULL;
}

static float RCAlias_ReadFloat (const unsigned char *source)
{
    float value;
    memcpy (&value, source, sizeof (value));
    return value;
}

rc_alias_mesh_t *RCAlias_CreateSkinnedStatic (uint32_t num_vertices, uint32_t num_indices,
    const void *vertices, size_t vertex_stride, uint32_t num_influences, uint32_t num_joints,
    const float *joint_rows, const uint16_t *indices)
{
    if (!num_vertices || num_vertices > 65536u || !num_indices || num_indices % 3 ||
        !vertices || !indices || !joint_rows || !num_joints || num_joints > 65536u ||
        (num_influences != 4 && num_influences != 8)) return NULL;
    const size_t position_offset = 32 + 2 * num_influences;
    const size_t minimum_stride = position_offset + 3 * num_influences * sizeof (float);
    if (vertex_stride < minimum_stride || (uint64_t)num_vertices > SIZE_MAX / vertex_stride ||
        (uint64_t)num_vertices * 3 * sizeof (float) > SIZE_MAX ||
        (uint64_t)num_vertices * 2 * sizeof (float) > SIZE_MAX ||
        (uint64_t)num_indices * sizeof (uint16_t) > SIZE_MAX) return NULL;
    for (uint32_t i = 0; i < num_indices; ++i) if (indices[i] >= num_vertices) return NULL;
    for (size_t i = 0; i < (size_t)num_joints * 12; ++i) if (!isfinite (joint_rows[i])) return NULL;
    size_t xyz_bytes = (size_t)num_vertices * 3 * sizeof (float);
    size_t uv_bytes = (size_t)num_vertices * 2 * sizeof (float);
    size_t index_bytes = (size_t)num_indices * sizeof (uint16_t);
    if (xyz_bytes > SIZE_MAX - uv_bytes || xyz_bytes + uv_bytes > SIZE_MAX - index_bytes ||
        xyz_bytes + uv_bytes + index_bytes > SIZE_MAX - sizeof (rc_alias_mesh_t)) return NULL;
    rc_alias_mesh_t *mesh = calloc (1, sizeof (*mesh));
    if (!mesh) return NULL;
    mesh->float_xyz = malloc (xyz_bytes); mesh->uv = malloc (uv_bytes); mesh->indices = malloc (index_bytes);
    if (!mesh->float_xyz || !mesh->uv || !mesh->indices) goto failed;
    for (uint32_t vertex = 0; vertex < num_vertices; ++vertex)
    {
        const unsigned char *raw = (const unsigned char *)vertices + (size_t)vertex * vertex_stride;
        float weights[8] = {0}, weight_sum = 0;
        /* Follow MD5_NormalizeJointWeights: UNORM bytes, then a common scale
         * over both groups. The sum need not equal255 after quantization. */
        for (uint32_t group = 0; group < num_influences / 4; ++group)
        {
            float group_sum = 0;
            for (uint32_t j = 0; j < 4; ++j)
            {
                uint32_t influence = group * 4 + j;
                weights[influence] = (float)raw[32 + influence] / 255.0f;
                group_sum += weights[influence];
            }
            weight_sum += group_sum;
        }
        if (weight_sum > 0)
        {
            float scale = 1.0f / weight_sum;
            for (uint32_t j = 0; j < num_influences; ++j) weights[j] *= scale;
        }
        float position[3] = {0};
        for (uint32_t j = 0; j < num_influences; ++j)
        {
            uint32_t joint = raw[32 + num_influences + j];
            if (joint >= num_joints) goto failed;
            const float *matrix = joint_rows + (size_t)joint * 12;
            float local[3];
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                local[axis] = RCAlias_ReadFloat (raw + position_offset + (axis * num_influences + j) * sizeof (float));
                if (!isfinite (local[axis])) goto failed;
            }
            /* Do not skip zero byte weights: their original weighted xyz
             * may be nonzero. Shader rotation uses xyz; only translation
             * uses the renormalized packed weight in the fourth component. */
            for (uint32_t axis = 0; axis < 3; ++axis)
                position[axis] += matrix[axis * 4] * local[0] + matrix[axis * 4 + 1] * local[1] +
                    matrix[axis * 4 + 2] * local[2] + matrix[axis * 4 + 3] * weights[j];
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            if (!isfinite (position[axis])) goto failed;
            mesh->float_xyz[(size_t)vertex * 3 + axis] = position[axis];
        }
        for (uint32_t axis = 0; axis < 2; ++axis)
        {
            float uv = RCAlias_ReadFloat (raw + 24 + axis * sizeof (float));
            if (!isfinite (uv)) goto failed;
            mesh->uv[(size_t)vertex * 2 + axis] = uv;
        }
    }
    memcpy (mesh->indices, indices, index_bytes);
    mesh->format = RC_ALIAS_SKINNED_STATIC;
    mesh->num_vertices = num_vertices; mesh->num_indices = num_indices; mesh->num_poses = 1;
    mesh->bytes = sizeof (*mesh) + xyz_bytes + uv_bytes + index_bytes;
    mesh->revision = Atomic_AddUInt64 (&rc_alias_next_revision, 1) + 1;
    if (!mesh->revision) mesh->revision = Atomic_AddUInt64 (&rc_alias_next_revision, 1) + 1;
    Atomic_AddUInt64 (&rc_alias_total_bytes, (uint64_t)mesh->bytes);
    return mesh;
failed:
    RCAlias_Destroy (mesh);
    return NULL;
}

void RCAlias_Destroy (rc_alias_mesh_t *mesh)
{
	if (!mesh) return;
	if (mesh->bytes) Atomic_SubUInt64 (&rc_alias_total_bytes, (uint64_t)mesh->bytes);
	free (mesh->xyz); free (mesh->indices); free (mesh->uv); free (mesh->float_xyz); free (mesh);
}

uint64_t RCAlias_Revision (const rc_alias_mesh_t *mesh) { return mesh ? mesh->revision : 0; }

size_t RCAlias_Bytes (const rc_alias_mesh_t *mesh) { return mesh ? mesh->bytes : 0; }
uint32_t RCAlias_TriangleCount (const rc_alias_mesh_t *mesh) { return mesh ? mesh->num_indices / 3 : 0; }
uint64_t RCAlias_TotalBytes (void) { return Atomic_LoadUInt64 (&rc_alias_total_bytes); }

bool RCAlias_Append (rc_scene_t *scene, const rc_alias_mesh_t *mesh, uint32_t pose1, uint32_t pose2,
	float blend, const float matrix[16], uint32_t material, uint32_t flags, uint32_t surface, uint32_t instance)
{
	if (!scene || !mesh || !matrix || pose1 >= mesh->num_poses || pose2 >= mesh->num_poses ||
		!isfinite (blend) || blend < 0 || blend > 1 || material == UINT32_MAX) return false;
	for (int i = 0; i < 16; ++i) if (!isfinite (matrix[i])) return false;
	uint32_t original = scene->triangle_count;
	for (uint32_t i = 0; i < mesh->num_indices; i += 3)
	{
		rc_scene_triangle_t tri = {0};
		for (uint32_t v = 0; v < 3; ++v)
		{
			uint32_t index = mesh->indices[i + v];
            float local[3];
            if (mesh->format == RC_ALIAS_SKINNED_STATIC)
                memcpy (local, mesh->float_xyz + (size_t)index * 3, sizeof (local));
            else
            {
			const uint16_t *a = mesh->xyz + ((size_t)pose1 * mesh->num_vertices + index) * 3;
			const uint16_t *b = mesh->xyz + ((size_t)pose2 * mesh->num_vertices + index) * 3;
			for (int axis = 0; axis < 3; ++axis)
			{
				float interpolated = (float)a[axis] * (1.0f - blend) + (float)b[axis] * blend;
				float decoded = mesh->format == RC_ALIAS_MDL ? interpolated * (255.0f / 65535.0f) : interpolated - 32768.0f;
				local[axis] = decoded * mesh->scale[axis] + mesh->scale_origin[axis];
			}
            }
			for (int axis = 0; axis < 3; ++axis)
				tri.position[v][axis] = matrix[axis] * local[0] + matrix[4+axis] * local[1] + matrix[8+axis] * local[2] + matrix[12+axis];
			tri.position[v][3] = 1.0f;
			float *outuv = v == 2 ? tri.uv2 : tri.uv01 + v * 2;
			outuv[0] = mesh->uv[index*2]; outuv[1] = mesh->uv[index*2+1];
		}
		tri.metadata[0] = material; tri.metadata[1] = flags | RC_SCENE_DYNAMIC;
		tri.metadata[2] = surface; tri.metadata[3] = instance;
		if (!RCScene_AppendTriangle (scene, &tri)) { scene->triangle_count = original; return false; }
	}
	return true;
}
