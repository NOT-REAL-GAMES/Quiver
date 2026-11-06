/* Source-editor model rendering. Include after quakedef.h. */
#ifndef QUIVER_EDITOR_MODEL_PREVIEW_H
#define QUIVER_EDITOR_MODEL_PREVIEW_H
#include "editor_map.h"

typedef struct
{
	float projection[16];
	int width, height, top, bottom;
} editor_model_view_t;

/* Prepare exact frame0 headers on the main thread each frame. Draw never
 * loads models and never owns their immutable mesh/texture/joint resources. */
void EditorModelPreview_BeginFrame (void);
void EditorModelPreview_Prepare (const editor_map_model_t *placement);
void EditorModelPreview_Clear (void);
qboolean EditorModelPreview_Perspective (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t camera, const vec3_t forward,
	const vec3_t right, const vec3_t up, float focal);
qboolean EditorModelPreview_Ortho (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t center, int horizontal, int vertical, int depth, float zoom);
qboolean EditorModelPreview_Draw (cb_context_t *cbx, const editor_map_model_t *placement,
	const editor_model_view_t *view);

/* Existing alias draw implementation, using cached headers and UI-compatible
 * pipelines, without world light samples, Radiance or world camera state. */
qboolean R_DrawEditorAlias (cb_context_t *cbx, qmodel_t *model, aliashdr_t *header,
	const editor_map_model_t *placement, const float projection[16]);
#endif
