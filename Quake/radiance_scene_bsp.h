/* Brush-model adapter for the radiance scene builder. */
#ifndef RADIANCE_SCENE_BSP_H
#define RADIANCE_SCENE_BSP_H
#include "radiance_scene.h"

/* Include gl_model.h before this header. Material identifiers must index the
 * GPU material table for both software and hardware tracing. The callback may
 * return UINT32_MAX to indicate an unavailable material. Matrix is column-major
 * model-to-world; NULL means identity. A nonzero dynamic flag marks moving
 * instances. The caller rebuilds the BVH after adding or moving geometry. */
typedef uint32_t (*rc_scene_material_fn) (const texture_t *texture, void *user);
bool RCScene_AppendBrush (rc_scene_t *scene, const qmodel_t *model, const float matrix[16],
	uint32_t instance, bool dynamic, rc_scene_material_fn material, void *user);

#endif
