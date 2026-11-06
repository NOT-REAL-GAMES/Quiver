#include "quakedef.h"
#include "editor_model_preview.h"

typedef struct
{
	char path[MAX_QPATH];
	int skin;
	qmodel_t *model;
	aliashdr_t *header;
} editor_model_asset_t;
static editor_model_asset_t *assets;
static size_t asset_count, asset_capacity;

void EditorModelPreview_BeginFrame (void) { asset_count = 0; }
void EditorModelPreview_Clear (void)
{
	free (assets); assets = NULL; asset_count = asset_capacity = 0;
}

void EditorModelPreview_Prepare (const editor_map_model_t *placement)
{
	if (!placement || strlen (placement->model) >= MAX_QPATH || placement->skin < 0) return;
	for (size_t i = 0; i < asset_count; ++i)
		if (assets[i].skin == placement->skin && !strcmp (assets[i].path, placement->model)) return;
	if (asset_count == asset_capacity)
	{
		size_t capacity = asset_capacity ? asset_capacity * 2 : 16;
		if (capacity > MAX_MODELS) return;
		editor_model_asset_t *next = realloc (assets, capacity * sizeof (*next));
		if (!next) return;
		assets = next; asset_capacity = capacity;
	}
	editor_model_asset_t *asset = &assets[asset_count++];
	memset (asset, 0, sizeof (*asset));
	q_strlcpy (asset->path, placement->model, sizeof (asset->path)); asset->skin = placement->skin;
	asset->model = Mod_ForName (placement->model, false);
	if (!asset->model || asset->model->type != mod_alias) return;
	aliashdr_t *header = Mod_Extradata_Exact (asset->model, placement->skin);
	if (!header) return;
	for (const aliashdr_t *surface = header; surface; surface = surface->nextsurface)
		if (surface->numframes != 1 || surface->numposes != 1 || !surface->radiance_mesh ||
			surface->vertex_buffer == VK_NULL_HANDLE || surface->index_buffer == VK_NULL_HANDLE ||
			((surface->poseverttype == PV_MD5 || surface->poseverttype == PV_MD5_8) && surface->joints_set == VK_NULL_HANDLE)) return;
	asset->header = header;
}

static qboolean EditorModelPreview_View (editor_model_view_t *view, int width, int height, int top, int bottom)
{
	if (!view || width <= 0 || height <= 0 || top < 0 || bottom > height || top >= bottom) return false;
	memset (view, 0, sizeof (*view));
	view->width = width; view->height = height; view->top = top; view->bottom = bottom;
	return true;
}

qboolean EditorModelPreview_Perspective (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t camera, const vec3_t forward,
	const vec3_t right, const vec3_t up, float focal)
{
	if (!EditorModelPreview_View (view, width, height, top, bottom) || !isfinite (focal) || focal <= 0) return false;
	/* Match the editor's screen coordinates and reversed depth 1/viewZ.
 * Leave clipW=viewZ so the GPU interpolates textures perspectively. */
	const float sx = 2 * focal / width, sy = -2 * focal / height;
	const float cy = (float)(top + bottom) / height - 1;
	for (unsigned a = 0; a < 3; ++a)
	{
		view->projection[a * 4] = sx * right[a];
		view->projection[a * 4 + 1] = sy * up[a] + cy * forward[a];
		view->projection[a * 4 + 3] = forward[a];
		view->projection[12] -= sx * right[a] * camera[a];
		view->projection[13] -= (sy * up[a] + cy * forward[a]) * camera[a];
		view->projection[15] -= forward[a] * camera[a];
	}
	view->projection[14] = 1; /* near plane is one world unit */
	return true;
}

qboolean EditorModelPreview_Ortho (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t center, int horizontal, int vertical, int depth, float zoom)
{
	if (!EditorModelPreview_View (view, width, height, top, bottom) || !isfinite (zoom) || zoom <= 0 ||
		horizontal < 0 || horizontal > 2 || vertical < 0 || vertical > 2 || depth < 0 || depth > 2 ||
		horizontal == vertical || horizontal == depth || vertical == depth) return false;
	view->projection[horizontal * 4] = 2 * zoom / width;
	view->projection[vertical * 4 + 1] = -2 * zoom / height;
	view->projection[depth * 4 + 2] = 1.f / 33554432.f;
	view->projection[12] = -2 * zoom * center[horizontal] / width;
	view->projection[13] = (float)(top + bottom) / height - 1 + 2 * zoom * center[vertical] / height;
	view->projection[14] = .5f; view->projection[15] = 1;
	return true;
}

qboolean EditorModelPreview_Draw (cb_context_t *cbx, const editor_map_model_t *placement,
	const editor_model_view_t *view)
{
	if (!cbx || !placement || !view || cbx->subpass_type != SUBPASS_UI) return false;
	for (size_t i = 0; i < asset_count; ++i)
	{
		const editor_model_asset_t *asset = &assets[i];
		if (asset->skin != placement->skin || strcmp (asset->path, placement->model)) continue;
		if (!asset->header || !asset->model || asset->model->needload || strcmp (asset->model->name, asset->path)) return false;
		const VkRect2D scissor = {{0, view->top}, {view->width, view->bottom - view->top}};
		vkCmdSetScissor (cbx->cb, 0, 1, &scissor);
		qboolean drawn = R_DrawEditorAlias (cbx, asset->model, asset->header, placement, view->projection);
		/* The alias layout has a larger push range than the canvas. Rebind
 * first, then restore the full canvas projection and viewport explicitly. */
		R_BindGraphicsPipeline (cbx, PIPELINE_BASIC_BLEND);
		cbx->current_canvas = CANVAS_NONE; GL_SetCanvas (cbx, CANVAS_DEFAULT);
		const VkRect2D full = {{0, 0}, {view->width, view->height}};
		vkCmdSetScissor (cbx->cb, 0, 1, &full);
		return drawn;
	}
	return false;
}
