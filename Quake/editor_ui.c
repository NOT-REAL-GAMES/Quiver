/* Source-map editor with perspective and orthographic viewports. */
#include "quakedef.h"
#include "editor_ui.h"
#include "editor_controls.h"
#include "editor_model_preview.h"
#include <float.h>
#include <math.h>

#define EDITOR_GRID 16.0f
#define EDITOR_HEADER 48.0f
#define EDITOR_FOOTER 32.0f
#define EDITOR_MAX_ZOOM 8.0f
#define EDITOR_MIN_ZOOM 0.0625f
#define EDITOR3D_MAX_FACE 64
#define EDITOR3D_MAX_CLIPPED 80
#define EDITOR3D_CLICK_RADIUS 8.0f
#define EDITOR3D_DRAG_THRESHOLD 4.0f
#define EDITOR3D_VERTEX_SNAP_PIXELS 14.0f
#define EDITOR3D_VERTEX_SNAP_DISTANCE 24.0f

typedef enum { EDIT_DRAG_NONE, EDIT_DRAG_PAN, EDIT_DRAG_CREATE, EDIT_DRAG_MOVE, EDIT_DRAG_RESIZE, EDIT_DRAG_FACE, EDIT_DRAG_VERTEX, EDIT_DRAG_POINT } editor_drag_t;
typedef enum { EDIT_SECTOR_VOLUME, EDIT_SECTOR_FACE, EDIT_SECTOR_VERTEX, EDIT_SECTOR_KNIFE } editor_sector_mode_t;
typedef enum { EDIT_ORTHO_TOP, EDIT_ORTHO_FRONT, EDIT_ORTHO_RIGHT } editor_ortho_view_t;
typedef struct
{
	qboolean valid, has_revision;
	int x, y, width, height, sector, face;
	uint64_t revision;
	vec3_t camera;
	float yaw, pitch;
} editor_face_click_t;
static struct
{
	qboolean active, ctrl, shift, perspective;
	editor_ui_adapter_t api;
	vec3_t center;
	float zoom;
	editor_ortho_view_t ortho_view;
	int mouse_x, mouse_y, start_x, start_y;
	float anchor_x, anchor_y;
	editor_drag_t drag;
	int drag_index;
	editor_sector_mode_t sector_mode;
	int create_prop_type;
	int sector_face, sector_vertex, portal_sector, portal_face;
	uint64_t portal_revision;
	vec3_t face_normal, vertex_origin, vertex_preview;
	vec3_t point_original, point_preview;
	uint64_t point_revision;
	qboolean vertex_snap, vertex_dragged;
	int vertex_snap_sector, vertex_snap_vertex;
	qboolean knife_pending, knife_preview_valid;
	int knife_sector, knife_face;
	uint64_t knife_revision;
	vec3_t knife_start, knife_preview;
	float face_distance;
	vec3_t original_mins, original_maxs, preview_mins, preview_maxs;
	vec3_t camera, drag_point, drag_normal;
	float yaw, pitch;
	qboolean move_forward, move_back, move_left, move_right, move_up, move_down;
	double last_update;
	char message[160];
	char message_session_status[256];
	double message_until;
	editor_face_click_t last_face_click, face_press;
	qboolean face_press_cycle, face_press_dragged;
} ui = {.zoom = 1.0f, .drag_index = -1, .sector_face = -1, .sector_vertex = -1,
	.portal_sector = -1, .portal_face = -1, .perspective = true, .camera = {0, 0, 80}};

static void Editor3D_Update (int x, int y);
static qboolean Editor3D_KeyEvent (int key, qboolean down);
static void Editor3D_Draw (cb_context_t *cbx);
static int Editor3D_Clip2D (float input[][3], int count, float output[][3], int axis, float edge, qboolean greater);
static qboolean Editor3D_SameFaceClickView (const editor_face_click_t *click);
static void EditorUI_ClearPortalEdges (void);
static qboolean Editor3D_PlaceLight (void);
static int Editor3D_PickPoint (int x, int y, vec3_t position);
static void Editor3D_Basis (vec3_t forward, vec3_t right, vec3_t up);
static float Editor3D_Focal (void);

static float EditorUI_Top (void) { return q_min (EDITOR_HEADER, (float)glheight); }
static float EditorUI_Bottom (void) { return q_max (EditorUI_Top (), glheight - EDITOR_FOOTER); }
static float EditorUI_CenterY (void) { return (EditorUI_Top () + EditorUI_Bottom ()) * 0.5f; }
static float EditorUI_Snap (float value) { return floorf (value / EDITOR_GRID + 0.5f) * EDITOR_GRID; }
static qboolean EditorUI_SectorMode (void) { return ui.api.sector_mode && ui.api.sector_mode (ui.api.context); }
static int EditorUI_HAxis (void) { return ui.ortho_view == EDIT_ORTHO_RIGHT ? 1 : 0; }
static int EditorUI_VAxis (void) { return ui.ortho_view == EDIT_ORTHO_TOP ? 1 : 2; }
static int EditorUI_DepthAxis (void) { return ui.ortho_view == EDIT_ORTHO_TOP ? 2 : ui.ortho_view == EDIT_ORTHO_FRONT ? 1 : 0; }
static const char *EditorUI_AxisName (int axis) { return axis == 0 ? "X" : axis == 1 ? "Y" : "Z"; }
/* Use the document's exact positions when available: unrelated vertices can
 * round to the same float in the viewport at large world coordinates. */
static qboolean EditorUI_VertexMovesWithDrag (int sector, int vertex)
{
	if (ui.drag != EDIT_DRAG_VERTEX) return false;
	if (ui.api.sector_vertex_coincident)
		return ui.api.sector_vertex_coincident (ui.api.context, sector, vertex, ui.drag_index, ui.sector_vertex);
	return sector == ui.drag_index && vertex == ui.sector_vertex;
}
static qboolean EditorUI_FaceVertexMovesWithDrag (int sector, int face, int corner, qboolean display_polygon, const vec3_t position)
{
	if (ui.drag != EDIT_DRAG_VERTEX) return false;
	if (ui.api.sector_face_vertex_coincident)
		return ui.api.sector_face_vertex_coincident (ui.api.context, sector, face, corner,
			ui.drag_index, ui.sector_vertex, display_polygon);
	vec3_t diff; VectorSubtract (position, ui.vertex_origin, diff);
	return sector == ui.drag_index && DotProduct (diff, diff) < 0.0001f;
}
static void EditorUI_ClearPendingPortal (void) { ui.portal_sector = ui.portal_face = -1; }
static void EditorUI_ResetFaceCycle (void)
{
	ui.last_face_click.valid = ui.face_press.valid = false;
	ui.face_press_cycle = ui.face_press_dragged = false;
}
editor_ui_view_t EditorUI_GetView (void)
{
	return ui.perspective ? EDITOR_VIEW_PERSPECTIVE : (editor_ui_view_t)(EDITOR_VIEW_TOP + ui.ortho_view);
}
void EditorUI_SetView (editor_ui_view_t view)
{
	if (view < EDITOR_VIEW_PERSPECTIVE || view > EDITOR_VIEW_RIGHT) return;
	ui.knife_pending = ui.knife_preview_valid = false;
	EditorUI_ResetFaceCycle (); EditorUI_ClearPendingPortal ();
	ui.perspective = view == EDITOR_VIEW_PERSPECTIVE;
	if (!ui.perspective) ui.ortho_view = (editor_ortho_view_t)(view - EDITOR_VIEW_TOP);
	ui.drag = EDIT_DRAG_NONE; ui.vertex_snap = false;
	ui.sector_face = ui.sector_vertex = -1;
	ui.move_forward = ui.move_back = ui.move_left = ui.move_right = ui.move_up = ui.move_down = false;
	ui.last_update = 0;
}

void EditorUI_ScreenToWorld (float x, float y, float *world_x, float *world_y)
{
	if (world_x) *world_x = ui.center[EditorUI_HAxis ()] + (x - glwidth * 0.5f) / ui.zoom;
	if (world_y) *world_y = ui.center[EditorUI_VAxis ()] + (EditorUI_CenterY () - y) / ui.zoom;
}
void EditorUI_WorldToScreen (float x, float y, float *screen_x, float *screen_y)
{
	if (screen_x) *screen_x = glwidth * 0.5f + (x - ui.center[EditorUI_HAxis ()]) * ui.zoom;
	if (screen_y) *screen_y = EditorUI_CenterY () - (y - ui.center[EditorUI_VAxis ()]) * ui.zoom;
}
void EditorUI_SetAdapter (const editor_ui_adapter_t *adapter)
{
	EditorUI_ClearPortalEdges ();
	ui.knife_pending = ui.knife_preview_valid = false;
	ui.api = adapter ? *adapter : (editor_ui_adapter_t){0};
	ui.drag = EDIT_DRAG_NONE;
	ui.vertex_snap = false;
	ui.sector_mode = EDIT_SECTOR_VOLUME;
	ui.create_prop_type = 0;
	ui.sector_face = ui.sector_vertex = ui.portal_sector = ui.portal_face = -1;
	EditorUI_ResetFaceCycle ();
}
void EditorUI_Focus (const vec3_t origin)
{
	ui.knife_pending = ui.knife_preview_valid = false;
	ui.vertex_snap = false;
	ui.camera[0] = origin[0]; ui.camera[1] = origin[1]; ui.camera[2] = origin[2] + 48;
	VectorCopy (origin, ui.center);
	ui.yaw = ui.pitch = 0;
	ui.sector_mode = EDIT_SECTOR_VOLUME;
	ui.sector_face = ui.sector_vertex = ui.portal_sector = ui.portal_face = -1;
	EditorUI_ResetFaceCycle ();
}
void EditorUI_SetActive (qboolean active)
{
	if (!active) EditorUI_ClearPortalEdges ();
	EditorControls_SetActive (active);
	ui.knife_pending = ui.knife_preview_valid = false;
	ui.active = active;
	ui.ctrl = ui.shift = false;
	ui.drag = EDIT_DRAG_NONE;
	ui.vertex_snap = false;
	ui.move_forward = ui.move_back = ui.move_left = ui.move_right = ui.move_up = ui.move_down = false;
	ui.last_update = 0;
	ui.message[0] = 0;
	EditorUI_ResetFaceCycle ();
}
qboolean EditorUI_IsActive (void) { return ui.active; }
void EditorUI_CancelDrag (void)
{
	EditorControls_CancelInput ();
	ui.knife_pending = ui.knife_preview_valid = false;
	ui.drag = EDIT_DRAG_NONE; ui.ctrl = ui.shift = false;
	ui.vertex_snap = false;
	ui.move_forward = ui.move_back = ui.move_left = ui.move_right = ui.move_up = ui.move_down = false;
	ui.last_update = 0;
	EditorUI_ResetFaceCycle ();
}
qboolean EditorUI_CancelKnife (void)
{
	if (!ui.active || !ui.perspective || ui.sector_mode != EDIT_SECTOR_KNIFE || !ui.knife_pending) return false;
	ui.knife_pending = ui.knife_preview_valid = false;
	EditorUI_SetStatus ("Knife cut cancelled");
	return true;
}
static const char *EditorUI_SessionStatus (void)
{
	const char *status = ui.api.status ? ui.api.status (ui.api.context) : NULL;
	return status ? status : "";
}
void EditorUI_SetStatus (const char *message)
{
	q_strlcpy (ui.message, message ? message : "", sizeof (ui.message));
	q_strlcpy (ui.message_session_status, EditorUI_SessionStatus (), sizeof (ui.message_session_status));
	ui.message_until = realtime + 5.0;
}
static const char *EditorUI_StatusText (void)
{
	const char *session = EditorUI_SessionStatus ();
	if (*ui.message && realtime < ui.message_until && !strcmp (session, ui.message_session_status)) return ui.message;
	return session;
}
static void EditorUI_FallbackError (const char *old_status, const char *message)
{
	/* An adapter may have provided a more specific validation error. */
	if (!strcmp (old_status, EditorUI_SessionStatus ())) EditorUI_SetStatus (message);
	else ui.message[0] = 0;
}

static qboolean EditorUI_InViewport (int x, int y)
{
	return x >= 0 && x < glwidth && y >= EditorUI_Top () && y < EditorUI_Bottom ();
}
static int EditorUI_Selected (void) { return ui.api.selected ? ui.api.selected (ui.api.context) : -1; }
static int EditorUI_PointSelected (void) { return ui.api.point_selected ? ui.api.point_selected (ui.api.context) : -1; }
static qboolean EditorUI_ModelBounds (int index,vec3_t corners[8])
{
	if(!ui.api.point_model_bounds || !ui.api.point_model_bounds(ui.api.context,index,corners))return false;
	if(ui.drag==EDIT_DRAG_POINT && ui.drag_index==index)
		for(unsigned v=0;v<8;++v)for(unsigned a=0;a<3;++a)corners[v][a]+=ui.point_preview[a]-ui.point_original[a];
	return true;
}
void EditorUI_GetInspectorState (editor_inspector_state_t *state)
{
	if (!state) return;
	memset (state, 0, sizeof (*state)); state->index = -1;
	state->supported = ui.api.sector_collision_enabled && ui.api.sector_set_collision_enabled;
	state->model_supported = ui.api.point_model && ui.api.point_set_model && ui.api.point_create_model;
	state->light_color_supported = ui.api.point_light_color && ui.api.point_set_light_color;
	state->supported |= state->model_supported || state->light_color_supported;
	state->script_supported = ui.api.world_script && ui.api.set_world_script;
	if (state->script_supported)
	{
		const char *script = ui.api.world_script (ui.api.context);
		q_strlcpy (state->world_script, script ? script : "", sizeof (state->world_script));
	}
	if (ui.api.sector_revision) state->revision = ui.api.sector_revision (ui.api.context);
	int point = EditorUI_PointSelected ();
	if (point >= 0 && ui.api.point_count && point < ui.api.point_count (ui.api.context))
	{
		state->kind = EDITOR_INSPECT_LIGHT; state->index = point;
		if (state->model_supported && ui.api.point_model (ui.api.context, point, &state->model))
		{ state->kind = EDITOR_INSPECT_MODEL; state->collision_enabled = state->model.collision; }
		if (state->kind == EDITOR_INSPECT_LIGHT && state->light_color_supported)
			state->light_color_supported = ui.api.point_light_color (ui.api.context, point, state->light_color);
		return;
	}
	int sector = EditorUI_Selected ();
	if (sector < 0 || !ui.api.brush_count || sector >= ui.api.brush_count (ui.api.context)) return;
	state->index = sector;
	if (!EditorUI_SectorMode ()) { state->kind = EDITOR_INSPECT_BRUSH; return; }
	int type = ui.api.sector_prop_type ? ui.api.sector_prop_type (ui.api.context, sector) : -1;
	if (type < 0 || type > 3) { state->index = -1; return; }
	state->kind = (editor_inspector_kind_t)(EDITOR_INSPECT_WORLD + type);
	if (state->kind == EDITOR_INSPECT_STATIC && state->supported)
		state->collision_enabled = ui.api.sector_collision_enabled (ui.api.context, sector);
}
qboolean EditorUI_SetInspectorScript (const editor_inspector_state_t *selection, const char *path)
{
	editor_inspector_state_t now; EditorUI_GetInspectorState (&now);
	if (!selection || !selection->script_supported || !now.script_supported ||
		(selection->kind != EDITOR_INSPECT_WORLD && selection->kind != EDITOR_INSPECT_BRUSH) || selection->kind != now.kind ||
		selection->index != now.index || selection->revision != now.revision || ui.drag != EDIT_DRAG_NONE)
	{
		EditorUI_SetStatus ("Selection changed; script was not modified");
		return false;
	}
	return ui.api.set_world_script (ui.api.context, path);
}
qboolean EditorUI_SetInspectorCollision (const editor_inspector_state_t *selection, qboolean enabled)
{
	editor_inspector_state_t now; EditorUI_GetInspectorState (&now);
	if (!selection || !selection->supported || !now.supported || (now.kind != EDITOR_INSPECT_STATIC && now.kind != EDITOR_INSPECT_MODEL) ||
		selection->kind != now.kind || selection->index != now.index || selection->revision != now.revision || ui.drag != EDIT_DRAG_NONE)
	{
		EditorUI_SetStatus ("Selection changed; collision was not modified");
		return false;
	}
	if (now.kind == EDITOR_INSPECT_MODEL)
	{ editor_map_model_t model = now.model; model.collision = enabled; return EditorUI_SetInspectorModel (selection, &model); }
	char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
	if (!ui.api.sector_set_collision_enabled (ui.api.context, now.index, enabled))
	{
		EditorUI_FallbackError (old_status, "Could not change collision");
		return false;
	}
	return true;
}
qboolean EditorUI_SetInspectorLightColor (const editor_inspector_state_t *selection, const double rgb[3])
{
	editor_inspector_state_t now; EditorUI_GetInspectorState (&now);
	if (!selection || !rgb || !selection->light_color_supported || !now.light_color_supported || now.kind != EDITOR_INSPECT_LIGHT ||
		selection->kind != now.kind || selection->index != now.index || selection->revision != now.revision || ui.drag != EDIT_DRAG_NONE)
	{ EditorUI_SetStatus ("Selection changed; light color was not modified"); return false; }
	for (unsigned i = 0; i < 3; ++i)
		if (!isfinite (rgb[i]) || rgb[i] < 0 || rgb[i] > 1)
		{ EditorUI_SetStatus ("Light RGB channels must be between 0 and 255"); return false; }
	return ui.api.point_set_light_color (ui.api.context, now.index, rgb);
}
qboolean EditorUI_SetInspectorModel (const editor_inspector_state_t *selection, const editor_map_model_t *model)
{
	editor_inspector_state_t now; EditorUI_GetInspectorState (&now);
	if (!selection || !model || !selection->model_supported || !now.model_supported || now.kind != EDITOR_INSPECT_MODEL ||
		selection->kind != now.kind || selection->index != now.index || selection->revision != now.revision || ui.drag != EDIT_DRAG_NONE)
	{ EditorUI_SetStatus ("Selection changed; model was not modified"); return false; }
	return ui.api.point_set_model (ui.api.context, now.index, model);
}
qboolean EditorUI_CreateModel (const char *path)
{
	if (!ui.api.point_create_model || ui.drag != EDIT_DRAG_NONE) return false;
	vec3_t origin;
	if (ui.perspective)
	{
		vec3_t forward, right, up; Editor3D_Basis (forward, right, up);
		VectorMA (ui.camera, 96, forward, origin);
	}
	else VectorCopy (ui.center, origin);
	for (unsigned axis = 0; axis < 3; ++axis) origin[axis] = EditorUI_Snap (origin[axis]);
	return ui.api.point_create_model (ui.api.context, path, origin);
}
static int EditorUI_PickPoint (float x, float y, vec3_t position)
{
	if (!ui.api.point_count || !ui.api.point_origin) return -1;
	int best = -1, count = ui.api.point_count (ui.api.context); float radius2 = 12.0f * 12.0f;
	for (int i = 0; i < count; ++i)
	{
		vec3_t point; float sx, sy;
		if (!ui.api.point_origin (ui.api.context, i, point)) continue;
		EditorUI_WorldToScreen (point[EditorUI_HAxis ()], point[EditorUI_VAxis ()], &sx, &sy);
		float dx = sx - x, dy = sy - y, d = dx * dx + dy * dy;
		if (d < radius2) { radius2 = d; best = i; VectorCopy (point, position); }
	}
	return best;
}
static const char *EditorUI_PropName (int type)
{
	static const char *const names[] = {"world", "prop_static", "prop_dynamic", "prop_movable"};
	return type >= 0 && type < (int)countof (names) ? names[type] : "unknown";
}
static void EditorUI_Create (const vec3_t mins, const vec3_t maxs)
{
	if (EditorUI_SectorMode () && ui.create_prop_type)
	{
		char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
		if (!ui.api.sector_create_prop || !ui.api.sector_create_prop (ui.api.context, mins, maxs, ui.create_prop_type))
			EditorUI_FallbackError (old_status, "Could not create sector prop");
	}
	else if (ui.api.create_box) ui.api.create_box (ui.api.context, mins, maxs);
}
static qboolean EditorUI_PropKey (int key)
{
	if (!EditorUI_SectorMode () || (key != 'g' && key != 'o')) return false;
	if (ui.drag != EDIT_DRAG_NONE) { EditorUI_SetStatus ("Finish the current drag first"); return true; }
	if (key == 'g')
	{
		if (!ui.api.sector_create_prop) { EditorUI_SetStatus ("Sector prop creation is unavailable"); return true; }
		ui.create_prop_type = (ui.create_prop_type + 1) % 4;
		char message[80]; q_snprintf (message, sizeof (message), "Create: %s", EditorUI_PropName (ui.create_prop_type));
		EditorUI_SetStatus (message); return true;
	}
	int sector = EditorUI_Selected ();
	if (sector < 0) { EditorUI_SetStatus ("Select a sector first"); return true; }
	if (!ui.api.sector_prop_type || !ui.api.sector_set_prop_type)
	{ EditorUI_SetStatus ("Sector prop types are unavailable"); return true; }
	int type = ui.api.sector_prop_type (ui.api.context, sector);
	if (type < 0 || type > 3) { EditorUI_SetStatus ("Invalid sector type"); return true; }
	char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
	if (!ui.api.sector_set_prop_type (ui.api.context, sector, (type + 1) % 4))
		EditorUI_FallbackError (old_status, "Could not change sector type");
	else
	{
		char message[80]; q_snprintf (message, sizeof (message), "Sector type: %s", EditorUI_PropName ((type + 1) % 4));
		ui.sector_face = ui.sector_vertex = -1;
		EditorUI_ClearPendingPortal (); EditorUI_ResetFaceCycle (); EditorUI_SetStatus (message);
	}
	return true;
}
static int EditorUI_Pick (float x, float y)
{
	const int h = EditorUI_HAxis (), v = EditorUI_VAxis ();
	int count = ui.api.brush_count && ui.api.brush_bounds ? ui.api.brush_count (ui.api.context) : 0;
	/* Draw order and hit priority agree, including overlapping brushes. */
	for (int i = count - 1; i >= 0; --i)
	{
		if (EditorUI_SectorMode () && ui.api.brush_face_count && ui.api.brush_face_vertices)
		{
			int faces = ui.api.brush_face_count (ui.api.context, i);
			for (int f = 0; f < faces; ++f)
			{
				vec3_t verts[EDITOR3D_MAX_FACE];
				int n = ui.api.brush_face_vertices (ui.api.context, i, f, verts, EDITOR3D_MAX_FACE);
				qboolean inside = false;
				if (n < 3 || n > EDITOR3D_MAX_FACE) continue;
				for (int a = 0, b = n - 1; a < n; b = a++)
				{
					float ay = verts[a][v], by = verts[b][v];
					if ((ay > y) != (by > y) && x < verts[a][h] + (y - ay) * (verts[b][h] - verts[a][h]) / (by - ay)) inside = !inside;
				}
				if (inside) return i;
			}
			continue;
		}
		vec3_t mins, maxs;
		if (ui.api.brush_bounds (ui.api.context, i, mins, maxs) && x >= mins[h] && x <= maxs[h] && y >= mins[v] && y <= maxs[v])
			return i;
	}
	return -1;
}
/* An orthographic vertex pick uses screen distance, preferring the selected
 * sector. Hidden-axis distance breaks ties for overlapping projections. */
static int EditorUI_PickVertex (int x, int y, int *vertex_out, vec3_t position)
{
	if (!ui.api.brush_count || !ui.api.sector_vertex_count || !ui.api.sector_vertex) return -1;
	int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
	int selected = EditorUI_Selected (), sectors = ui.api.brush_count (ui.api.context);
	for (int pass = 0; pass < 2; ++pass)
	{
		int best_sector = -1, best_vertex = -1; float best = 14.0f * 14.0f, best_depth = FLT_MAX;
		for (int sector = 0; sector < sectors; ++sector)
		{
			if ((pass == 0 && sector != selected) || (pass == 1 && sector == selected)) continue;
			int count = ui.api.sector_vertex_count (ui.api.context, sector);
			for (int vertex = 0; vertex < count; ++vertex)
			{
				vec3_t point; float sx, sy;
				if (!ui.api.sector_vertex (ui.api.context, sector, vertex, point)) continue;
				EditorUI_WorldToScreen (point[h], point[v], &sx, &sy);
				float dx = sx - x, dy = sy - y, distance = dx * dx + dy * dy;
				float z = fabsf (point[depth] - ui.center[depth]);
				if (distance > best || (distance == best && z >= best_depth)) continue;
				best = distance; best_depth = z; best_sector = sector; best_vertex = vertex; VectorCopy (point, position);
			}
		}
		if (best_sector >= 0) { *vertex_out = best_vertex; return best_sector; }
	}
	return -1;
}
static qboolean EditorUI_SnapOrthoVertex (const vec3_t raw, vec3_t out)
{
	ui.vertex_snap = false;
	if (!ui.api.brush_count || !ui.api.sector_vertex_count || !ui.api.sector_vertex) return false;
	int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
	float best = EDITOR3D_VERTEX_SNAP_DISTANCE * EDITOR3D_VERTEX_SNAP_DISTANCE;
	int sectors = ui.api.brush_count (ui.api.context);
	for (int sector = 0; sector < sectors; ++sector)
	{
		int count = ui.api.sector_vertex_count (ui.api.context, sector);
		for (int vertex = 0; vertex < count; ++vertex)
		{
			vec3_t point;
			if ((sector == ui.drag_index && vertex == ui.sector_vertex) ||
				!ui.api.sector_vertex (ui.api.context, sector, vertex, point)) continue;
			if (!isfinite (point[0]) || !isfinite (point[1]) || !isfinite (point[2])) continue;
			if (ui.api.sector_vertex_coincident && ui.api.sector_vertex_coincident (ui.api.context, ui.drag_index, ui.sector_vertex, sector, vertex)) continue;
			vec3_t source_offset; VectorSubtract (point, ui.vertex_origin, source_offset);
			if (ui.api.sector_vertex_coincident && DotProduct (source_offset, source_offset) == 0) continue;
			if (!ui.api.sector_vertex_coincident && DotProduct (source_offset, source_offset) < 0.000001f &&
				(!ui.api.sector_same_vertex || ui.api.sector_same_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, sector, vertex))) continue;
			/* A 2D view cannot establish a different hidden coordinate. */
			if (ui.api.sector_vertex_same_axis ?
				!ui.api.sector_vertex_same_axis (ui.api.context, ui.drag_index, ui.sector_vertex, sector, vertex, depth) :
				fabsf (point[depth] - raw[depth]) > 0.0001f) continue;
			float dx = point[h] - raw[h], dy = point[v] - raw[v], distance = dx * dx + dy * dy;
			if (distance >= best || distance * ui.zoom * ui.zoom > EDITOR3D_VERTEX_SNAP_PIXELS * EDITOR3D_VERTEX_SNAP_PIXELS) continue;
			best = distance; VectorCopy (point, out); ui.vertex_snap = true;
			ui.vertex_snap_sector = sector; ui.vertex_snap_vertex = vertex;
		}
	}
	return ui.vertex_snap;
}
static void EditorUI_UpdateDrag (void)
{
	const int h = EditorUI_HAxis (), v = EditorUI_VAxis ();
	float raw_x, raw_y;
	EditorUI_ScreenToWorld ((float)ui.mouse_x, (float)ui.mouse_y, &raw_x, &raw_y);
	float x = EditorUI_Snap (raw_x), y = EditorUI_Snap (raw_y);
	if (ui.drag == EDIT_DRAG_VERTEX)
	{
		float mx = ui.mouse_x - ui.start_x, my = ui.mouse_y - ui.start_y;
		if (mx * mx + my * my > EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD) ui.vertex_dragged = true;
		if (!ui.vertex_dragged) { VectorCopy (ui.vertex_origin, ui.vertex_preview); ui.vertex_snap = false; return; }
		vec3_t raw; VectorCopy (ui.vertex_origin, raw);
		raw[h] += raw_x - ui.anchor_x; raw[v] += raw_y - ui.anchor_y;
		if (!EditorUI_SnapOrthoVertex (raw, ui.vertex_preview))
		{
			VectorCopy (ui.vertex_origin, ui.vertex_preview);
			if (fabsf (raw[h] - ui.vertex_origin[h]) > 0.0001f) ui.vertex_preview[h] = EditorUI_Snap (raw[h]);
			if (fabsf (raw[v] - ui.vertex_origin[v]) > 0.0001f) ui.vertex_preview[v] = EditorUI_Snap (raw[v]);
		}
	}
	else if (ui.drag == EDIT_DRAG_POINT)
	{
		VectorCopy (ui.point_original, ui.point_preview);
		float mx = ui.mouse_x - ui.start_x, my = ui.mouse_y - ui.start_y;
		if (mx * mx + my * my <= EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD) return;
		float dx = raw_x - ui.anchor_x, dy = raw_y - ui.anchor_y;
		if (fabsf (dx) > 0.0001f) ui.point_preview[h] = EditorUI_Snap (ui.point_original[h] + dx);
		if (fabsf (dy) > 0.0001f) ui.point_preview[v] = EditorUI_Snap (ui.point_original[v] + dy);
	}
	else if (ui.drag == EDIT_DRAG_CREATE)
	{
		ui.preview_mins[h] = q_min (ui.anchor_x, x); ui.preview_maxs[h] = q_max (ui.anchor_x, x);
		ui.preview_mins[v] = q_min (ui.anchor_y, y); ui.preview_maxs[v] = q_max (ui.anchor_y, y);
	}
	else if (ui.drag == EDIT_DRAG_MOVE)
	{
		VectorCopy (ui.original_mins, ui.preview_mins); VectorCopy (ui.original_maxs, ui.preview_maxs);
		float mx = ui.mouse_x - ui.start_x, my = ui.mouse_y - ui.start_y;
		if (mx * mx + my * my <= EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD) return;
		float dx = raw_x - ui.anchor_x, dy = raw_y - ui.anchor_y;
		dx = fabsf (dx) > 0.0001f ? EditorUI_Snap (ui.original_mins[h] + dx) - ui.original_mins[h] : 0;
		dy = fabsf (dy) > 0.0001f ? EditorUI_Snap (ui.original_mins[v] + dy) - ui.original_mins[v] : 0;
		ui.preview_mins[h] += dx; ui.preview_maxs[h] += dx;
		ui.preview_mins[v] += dy; ui.preview_maxs[v] += dy;
	}
	else if (ui.drag == EDIT_DRAG_RESIZE)
	{
		VectorCopy (ui.original_mins, ui.preview_mins); VectorCopy (ui.original_maxs, ui.preview_maxs);
		ui.preview_maxs[h] = q_max (ui.preview_mins[h] + EDITOR_GRID, x);
		ui.preview_mins[v] = q_min (ui.preview_maxs[v] - EDITOR_GRID, y);
	}
}
void EditorUI_Update (void)
{
	if (!ui.active) return;
	if (ui.drag == EDIT_DRAG_POINT && ui.api.sector_revision && ui.point_revision != ui.api.sector_revision (ui.api.context))
	{
		ui.drag = EDIT_DRAG_NONE;
		EditorUI_SetStatus ("Light move cancelled: document changed");
	}
	if (ui.last_face_click.valid && !Editor3D_SameFaceClickView (&ui.last_face_click)) ui.last_face_click.valid = false;
	if (ui.face_press.valid && !Editor3D_SameFaceClickView (&ui.face_press))
	{
		ui.face_press.valid = false;
		ui.last_face_click.valid = false;
	}
	if (ui.portal_sector >= 0 && ui.api.sector_revision && ui.portal_revision != ui.api.sector_revision (ui.api.context))
	{
		EditorUI_ClearPendingPortal ();
		ui.message[0] = 0;
	}
	if (ui.knife_pending && ui.api.sector_revision && ui.knife_revision != ui.api.sector_revision (ui.api.context))
	{
		ui.knife_pending = ui.knife_preview_valid = false;
		EditorUI_SetStatus ("Knife cut cancelled: sector changed");
	}
	int x, y;
	IN_GetMousePos (&x, &y);
	EditorControls_Update (x, y);
	if (ui.perspective)
	{
		Editor3D_Update (x, y);
		ui.mouse_x = x; ui.mouse_y = y;
		return;
	}
	if (ui.drag == EDIT_DRAG_PAN)
	{
		ui.center[EditorUI_HAxis ()] -= (x - ui.mouse_x) / ui.zoom;
		ui.center[EditorUI_VAxis ()] += (y - ui.mouse_y) / ui.zoom;
	}
	ui.mouse_x = x; ui.mouse_y = y;
	if (ui.drag != EDIT_DRAG_NONE && ui.drag != EDIT_DRAG_PAN)
		EditorUI_UpdateDrag ();
}

qboolean EditorUI_KeyEvent (int key, qboolean down)
{
	if (!ui.active) return false;
	EditorUI_Update ();
	if (down && key != K_CTRL && key != K_SHIFT) ui.message[0] = 0;
	if (key == K_CTRL) { ui.ctrl = down; return true; }
	if (key == K_SHIFT) { ui.shift = down; return true; }
	if ((key == K_MOUSE1 || key == K_MOUSE2 || key == K_MOUSE3) && (ui.drag == EDIT_DRAG_NONE || !down) &&
		EditorControls_MouseButton (key == K_MOUSE1 ? 1 : key == K_MOUSE2 ? 2 : 3, down, ui.mouse_x, ui.mouse_y)) return true;
	if (down && (key == K_MWHEELUP || key == K_MWHEELDOWN) &&
		EditorControls_Scroll (ui.mouse_x, ui.mouse_y, key == K_MWHEELUP ? -48 : 48)) return true;
	if (EditorControls_Key (key, down, ui.ctrl, ui.shift)) return true;
	if (down && !ui.ctrl && key == 'i')
	{
		if (ui.drag != EDIT_DRAG_NONE) EditorUI_SetStatus ("Finish the current drag first");
		else if (EditorControls_ToggleInspector ()) EditorUI_SetStatus ("Properties inspector toggled");
		return true;
	}
	if (down && ui.ctrl && key == 'd' && ui.api.duplicate_selected)
	{
		if (ui.drag != EDIT_DRAG_NONE) EditorUI_SetStatus ("Finish the current drag first");
		else { EditorUI_ClearPendingPortal (); ui.sector_face = ui.sector_vertex = -1; ui.api.duplicate_selected (ui.api.context); }
		return true;
	}
	if (down && !ui.ctrl && key == 'h' && ui.api.point_create_light && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
	{
		if (ui.drag != EDIT_DRAG_NONE) { EditorUI_SetStatus ("Finish the current drag first"); return true; }
		if (ui.perspective) Editor3D_PlaceLight ();
		else
		{
			int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
			vec3_t point, mins, maxs; EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &point[h], &point[v]);
			point[h] = EditorUI_Snap (point[h]); point[v] = EditorUI_Snap (point[v]);
			int sector = EditorUI_Selected ();
			point[depth] = sector >= 0 && ui.api.brush_bounds && ui.api.brush_bounds (ui.api.context, sector, mins, maxs) ?
				(mins[depth] + maxs[depth]) * 0.5f : (ui.ortho_view == EDIT_ORTHO_TOP ? ui.camera[2] : ui.center[depth]);
			point[depth] = EditorUI_Snap (point[depth]); ui.api.point_create_light (ui.api.context, point);
		}
		EditorUI_ClearPendingPortal (); ui.sector_face = ui.sector_vertex = -1; return true;
	}
	if (down && !ui.ctrl && EditorUI_PointSelected () >= 0)
	{
		int point = EditorUI_PointSelected ();
		if (key == K_DEL && ui.api.point_remove) { ui.api.point_remove (ui.api.context, point); return true; }
		if ((key == '[' || key == ']') && ui.api.point_origin && ui.api.point_set_origin)
		{
			vec3_t origin; if (ui.api.point_origin (ui.api.context, point, origin))
			{ int axis = ui.perspective ? 2 : EditorUI_DepthAxis (); origin[axis] += key == ']' ? EDITOR_GRID : -EDITOR_GRID; ui.api.point_set_origin (ui.api.context, point, origin); }
			return true;
		}
	}
	if (down && !ui.ctrl && (key == K_TAB || (key >= '1' && key <= '4')))
	{
		if (key == K_TAB) EditorUI_SetView ((editor_ui_view_t)((EditorUI_GetView () + 1) % 4));
		else EditorUI_SetView (key == '4' ? EDITOR_VIEW_PERSPECTIVE : (editor_ui_view_t)(EDITOR_VIEW_TOP + key - '1'));
		return true;
	}
	if (down && !ui.ctrl && EditorUI_PropKey (key)) return true;
	if (ui.perspective)
		return Editor3D_KeyEvent (key, down);
	if (down && !ui.ctrl && key == 'v' && EditorUI_SectorMode ())
	{
		if (ui.drag != EDIT_DRAG_NONE) { EditorUI_SetStatus ("Finish the current drag first"); return true; }
		ui.sector_mode = ui.sector_mode == EDIT_SECTOR_VERTEX ? EDIT_SECTOR_VOLUME : EDIT_SECTOR_VERTEX;
		ui.sector_face = ui.sector_vertex = -1; EditorUI_ClearPendingPortal ();
		EditorUI_SetStatus (ui.sector_mode == EDIT_SECTOR_VERTEX ? "Vertex mode" : "Sector mode"); return true;
	}
	if (key == K_MOUSE2 || key == K_MOUSE3)
	{
		if (down && ui.drag == EDIT_DRAG_NONE && EditorUI_InViewport (ui.mouse_x, ui.mouse_y)) ui.drag = EDIT_DRAG_PAN;
		else if (!down && ui.drag == EDIT_DRAG_PAN) ui.drag = EDIT_DRAG_NONE;
		return true;
	}
	if ((key == K_MWHEELUP || key == K_MWHEELDOWN) && down && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
	{
		float before_x, before_y, after_x, after_y;
		EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &before_x, &before_y);
		ui.zoom = CLAMP (EDITOR_MIN_ZOOM, ui.zoom * (key == K_MWHEELUP ? 1.25f : 0.8f), EDITOR_MAX_ZOOM);
		EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &after_x, &after_y);
		ui.center[EditorUI_HAxis ()] += before_x - after_x; ui.center[EditorUI_VAxis ()] += before_y - after_y;
		return true;
	}
	if (key == K_MOUSE1)
	{
		int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
		if (down && ui.drag == EDIT_DRAG_NONE && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
		{
			vec3_t light = {0,0,0}; int point = ui.shift ? -1 : EditorUI_PickPoint (ui.mouse_x, ui.mouse_y, light);
			if (point >= 0 && ui.api.point_select)
			{
				ui.api.point_select (ui.api.context, point); EditorUI_ClearPendingPortal (); ui.sector_face = ui.sector_vertex = -1;
				ui.drag_index = point; ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y;
				EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &ui.anchor_x, &ui.anchor_y);
				VectorCopy (light, ui.point_original); VectorCopy (light, ui.point_preview);
				ui.point_revision = ui.api.sector_revision ? ui.api.sector_revision (ui.api.context) : 0;
				ui.drag = EDIT_DRAG_POINT; return true;
			}
			if (!ui.shift && EditorUI_SectorMode () && ui.sector_mode == EDIT_SECTOR_VERTEX)
			{
				int vertex = -1; vec3_t position = {0, 0, 0};
				int sector = EditorUI_PickVertex (ui.mouse_x, ui.mouse_y, &vertex, position);
				if (sector >= 0)
				{
					if (ui.api.select) ui.api.select (ui.api.context, sector);
					ui.drag_index = sector; ui.sector_vertex = vertex; ui.sector_face = -1;
					ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y; ui.vertex_dragged = ui.vertex_snap = false;
					EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &ui.anchor_x, &ui.anchor_y);
					VectorCopy (position, ui.vertex_origin); VectorCopy (position, ui.vertex_preview);
					ui.drag = EDIT_DRAG_VERTEX; return true;
				}
			}
			float x, y, hx, hy;
			EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &x, &y);
			ui.anchor_x = EditorUI_Snap (x); ui.anchor_y = EditorUI_Snap (y);
			ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y;
			int hit = ui.shift ? -1 : EditorUI_Pick (x, y), old_selected = EditorUI_Selected ();
			ui.drag_index = hit;
			if (ui.api.select) ui.api.select (ui.api.context, hit);
			if (hit >= 0 && ui.api.brush_bounds && ui.api.brush_bounds (ui.api.context, hit, ui.original_mins, ui.original_maxs))
			{
				VectorCopy (ui.original_mins, ui.preview_mins); VectorCopy (ui.original_maxs, ui.preview_maxs);
				EditorUI_WorldToScreen (ui.original_maxs[h], ui.original_mins[v], &hx, &hy);
				ui.drag = (!EditorUI_SectorMode () || ui.sector_mode == EDIT_SECTOR_VOLUME) && hit == old_selected &&
					fabsf (hx - ui.mouse_x) <= 10 && fabsf (hy - ui.mouse_y) <= 10 ? EDIT_DRAG_RESIZE : EDIT_DRAG_MOVE;
				if (ui.drag == EDIT_DRAG_MOVE) { ui.anchor_x = x; ui.anchor_y = y; }
			}
			else
			{
				ui.preview_mins[h] = ui.preview_maxs[h] = ui.anchor_x;
				ui.preview_mins[v] = ui.preview_maxs[v] = ui.anchor_y;
				if (ui.ortho_view == EDIT_ORTHO_TOP)
				{ ui.preview_mins[depth] = EditorUI_SectorMode () ? 0 : -64; ui.preview_maxs[depth] = EditorUI_SectorMode () ? 128 : 64; }
				else
				{
					vec3_t mins, maxs;
					float middle = old_selected >= 0 && ui.api.brush_bounds && ui.api.brush_bounds (ui.api.context, old_selected, mins, maxs) ?
						(mins[depth] + maxs[depth]) * 0.5f : ui.center[depth];
					middle = EditorUI_Snap (middle);
					ui.preview_mins[depth] = middle - 64; ui.preview_maxs[depth] = middle + 64;
				}
				ui.drag = EDIT_DRAG_CREATE;
			}
		}
		else if (!down && ui.drag != EDIT_DRAG_NONE && ui.drag != EDIT_DRAG_PAN)
		{
			EditorUI_UpdateDrag ();
			if (ui.drag == EDIT_DRAG_POINT && ui.api.point_set_origin && memcmp (ui.point_original, ui.point_preview, sizeof (vec3_t)))
				ui.api.point_set_origin (ui.api.context, ui.drag_index, ui.point_preview);
			else if (ui.drag == EDIT_DRAG_VERTEX &&
				(memcmp (ui.vertex_origin, ui.vertex_preview, sizeof (vec3_t)) || (ui.vertex_snap && ui.api.sector_merge_vertex)) &&
				(ui.api.sector_set_vertex || (ui.vertex_snap && ui.api.sector_merge_vertex)))
			{
				char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
				qboolean ok = ui.vertex_snap && ui.api.sector_merge_vertex ?
					ui.api.sector_merge_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, ui.vertex_snap_sector, ui.vertex_snap_vertex) :
					ui.api.sector_set_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, ui.vertex_preview);
				if (!ok) EditorUI_FallbackError (old_status, "Vertex move rejected; it must preserve valid sectors and portals");
				else { ui.sector_face = ui.sector_vertex = -1; EditorUI_ClearPendingPortal (); }
			}
			else if (ui.drag == EDIT_DRAG_CREATE && ui.preview_maxs[h] > ui.preview_mins[h] && ui.preview_maxs[v] > ui.preview_mins[v])
				EditorUI_Create (ui.preview_mins, ui.preview_maxs);
			else if ((ui.drag == EDIT_DRAG_MOVE || ui.drag == EDIT_DRAG_RESIZE) && ui.api.set_bounds &&
				(memcmp (ui.preview_mins, ui.original_mins, sizeof (vec3_t)) || memcmp (ui.preview_maxs, ui.original_maxs, sizeof (vec3_t))))
				ui.api.set_bounds (ui.api.context, ui.drag_index, ui.preview_mins, ui.preview_maxs);
			ui.drag = EDIT_DRAG_NONE;
			ui.vertex_snap = false;
		}
		return true;
	}
	if (!down) return true;
	if (ui.ctrl && ui.api.action)
	{
		if (key == 's') ui.api.action (ui.api.context, EDITOR_UI_SAVE);
		else if (key == 'z') { EditorUI_ClearPendingPortal (); ui.api.action (ui.api.context, ui.shift ? EDITOR_UI_REDO : EDITOR_UI_UNDO); }
		else if (key == 'y') { EditorUI_ClearPendingPortal (); ui.api.action (ui.api.context, EDITOR_UI_REDO); }
		else if (key == 'p') ui.api.action (ui.api.context, EDITOR_UI_PLAY);
		return true;
	}
	if (key == K_DEL && ui.api.remove_selected)
	{
		EditorUI_ClearPendingPortal (); ui.sector_face = ui.sector_vertex = -1; ui.api.remove_selected (ui.api.context);
	}
	else if (key == 'p' && ui.api.place_spawn)
	{
		float x, y; vec3_t origin;
		EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &x, &y);
		int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
		origin[h] = EditorUI_Snap (x); origin[v] = EditorUI_Snap (y);
		origin[depth] = ui.ortho_view == EDIT_ORTHO_TOP ? 32 : EditorUI_Snap (ui.center[depth]);
		ui.api.place_spawn (ui.api.context, origin);
	}
	else if ((key == '[' || key == ']') && ui.api.brush_bounds && ui.api.set_bounds)
	{
		int selected = EditorUI_Selected (); vec3_t mins, maxs;
		if (selected >= 0 && ui.api.brush_bounds (ui.api.context, selected, mins, maxs))
		{
			float delta = key == ']' ? EDITOR_GRID : -EDITOR_GRID;
			int axis = EditorUI_DepthAxis ();
			if (ui.shift) maxs[axis] = q_max (mins[axis] + EDITOR_GRID, maxs[axis] + delta);
			else { mins[axis] += delta; maxs[axis] += delta; }
			ui.api.set_bounds (ui.api.context, selected, mins, maxs);
		}
	}
	return true;
}

/* Clamp every rectangle to the viewport because Draw_Fill has no scissor. */
static void EditorUI_FillClipped (cb_context_t *cbx, float x, float y, float w, float h, int color, float alpha)
{
	float right = q_min ((float)glwidth, x + w), bottom = q_min (EditorUI_Bottom (), y + h);
	x = q_max (0.0f, x); y = q_max (EditorUI_Top (), y);
	if (right > x && bottom > y) Draw_Fill (cbx, x, y, right - x, bottom - y, color, alpha);
}
static void EditorUI_DrawBox (cb_context_t *cbx, const vec3_t mins, const vec3_t maxs, qboolean selected)
{
	int h = EditorUI_HAxis (), v = EditorUI_VAxis ();
	float left, top, right, bottom;
	EditorUI_WorldToScreen (mins[h], maxs[v], &left, &top);
	EditorUI_WorldToScreen (maxs[h], mins[v], &right, &bottom);
	int color = selected ? 111 : 152;
	EditorUI_FillClipped (cbx, left, top, right - left, bottom - top, color, selected ? 0.24f : 0.12f);
	EditorUI_FillClipped (cbx, left, top, right - left, 2, color, 1);
	EditorUI_FillClipped (cbx, left, bottom - 2, right - left, 2, color, 1);
	EditorUI_FillClipped (cbx, left, top, 2, bottom - top, color, 1);
	EditorUI_FillClipped (cbx, right - 2, top, 2, bottom - top, color, 1);
	if (selected) EditorUI_FillClipped (cbx, right - 5, bottom - 5, 10, 10, color, 1);
}
static void EditorUI_DrawTopLine (cb_context_t *cbx, float x0, float y0, float x1, float y1, int color)
{
	float dx = x1 - x0, dy = y1 - y0, length = sqrtf (dx * dx + dy * dy);
	if (length < 0.01f) return;
	float nx = -dy / length, ny = dx / length;
	float a[EDITOR3D_MAX_CLIPPED][3] = {{x0 + nx,y0 + ny,1},{x1 + nx,y1 + ny,1},{x1 - nx,y1 - ny,1},{x0 - nx,y0 - ny,1}};
	float b[EDITOR3D_MAX_CLIPPED][3];
	int n = Editor3D_Clip2D (a, 4, b, 0, 0, true); if (n < 3) return;
	n = Editor3D_Clip2D (b, n, a, 0, glwidth, false); if (n < 3) return;
	n = Editor3D_Clip2D (a, n, b, 1, EditorUI_Top (), true); if (n < 3) return;
	n = Editor3D_Clip2D (b, n, a, 1, EditorUI_Bottom (), false); if (n < 3) return;
	Draw_EditorPolygonDepth (cbx, (const float (*)[3])a, n, color, 1, false);
}
static void EditorUI_DrawOrthoVertices (cb_context_t *cbx, int sector)
{
	if (sector < 0 || !ui.api.sector_vertex_count || !ui.api.sector_vertex) return;
	int h = EditorUI_HAxis (), v = EditorUI_VAxis ();
	int count = ui.api.sector_vertex_count (ui.api.context, sector);
	for (int i = 0; i < count; ++i)
	{
		vec3_t point; float x, y;
		if (!ui.api.sector_vertex (ui.api.context, sector, i, point)) continue;
		if (EditorUI_VertexMovesWithDrag (sector, i)) VectorCopy (ui.vertex_preview, point);
		EditorUI_WorldToScreen (point[h], point[v], &x, &y);
		EditorUI_DrawTopLine (cbx, x - 4, y, x + 4, y, i == ui.sector_vertex ? 111 : 15);
		EditorUI_DrawTopLine (cbx, x, y - 4, x, y + 4, i == ui.sector_vertex ? 111 : 15);
	}
}
/* The outlines of adjacent portal fragments describe one opening. Cache the
 * source edges by document revision, rather than comparing every face pair
 * on every frame. The underlying editable faces are never changed. */
typedef struct { double first, last; } editor_edge_range_t;
typedef struct
{
	int face, vertex, peer;
	vec3_t a, b;
	double normal[3];
	editor_edge_range_t *ranges;
	int nranges;
	qboolean resolved;
} editor_portal_edge_t;
typedef struct
{
	editor_portal_edge_t *edges, **order;
	int *offsets, faces;
	size_t count;
	qboolean built;
} editor_portal_sector_t;
static struct
{
	editor_portal_sector_t *sectors;
	int count;
	uint64_t revision;
	void *context;
} portal_edge_cache;

static void EditorUI_ClearPortalEdges (void)
{
	for (int i = 0; i < portal_edge_cache.count; ++i)
	{
		for (size_t j = 0; j < portal_edge_cache.sectors[i].count; ++j)
			free (portal_edge_cache.sectors[i].edges[j].ranges);
		free (portal_edge_cache.sectors[i].edges);
		free (portal_edge_cache.sectors[i].order);
		free (portal_edge_cache.sectors[i].offsets);
	}
	free (portal_edge_cache.sectors);
	memset (&portal_edge_cache, 0, sizeof (portal_edge_cache));
}
static void EditorUI_PreparePortalEdges (void)
{
	if (!EditorUI_SectorMode () || !ui.api.sector_face_portal_peer || !ui.api.sector_face_id || !ui.api.sector_source_face_vertices)
	{
		EditorUI_ClearPortalEdges ();
		return;
	}
	int count = ui.api.brush_count ? ui.api.brush_count (ui.api.context) : 0;
	uint64_t revision = ui.api.sector_revision ? ui.api.sector_revision (ui.api.context) : 0;
	if (count < 0 || count > 65536) count = 0;
	if (!ui.api.sector_revision || portal_edge_cache.count != count ||
		portal_edge_cache.context != ui.api.context || portal_edge_cache.revision != revision)
		EditorUI_ClearPortalEdges ();
	if (!portal_edge_cache.sectors && count)
	{
		portal_edge_cache.sectors = calloc ((size_t)count, sizeof (*portal_edge_cache.sectors));
		if (!portal_edge_cache.sectors) return;
		portal_edge_cache.count = count;
		portal_edge_cache.context = ui.api.context;
		portal_edge_cache.revision = revision;
	}
}
static int EditorUI_PortalEdgeOrder (const void *a, const void *b)
{
	const editor_portal_edge_t *x = *(editor_portal_edge_t *const *)a, *y = *(editor_portal_edge_t *const *)b;
	return (x->peer > y->peer) - (x->peer < y->peer);
}
static editor_portal_sector_t *EditorUI_PortalEdges (int sector)
{
	if (!ui.api.sector_face_portal_peer || !ui.api.sector_face_id || !ui.api.sector_source_face_vertices ||
		!ui.api.brush_face_count || sector < 0 || sector >= portal_edge_cache.count) return NULL;
	editor_portal_sector_t *cache = &portal_edge_cache.sectors[sector];
	if (cache->built) return cache;
	cache->built = true;
	int faces = ui.api.brush_face_count (ui.api.context, sector);
	if (faces <= 0 || faces > 131072) return cache;
	cache->offsets = malloc ((size_t)faces * sizeof (*cache->offsets));
	qboolean *seen = calloc ((size_t)faces, sizeof (*seen));
	if (!cache->offsets || !seen) { free (seen); free (cache->offsets); cache->offsets = NULL; return cache; }
	cache->faces = faces;
	for (int i = 0; i < faces; ++i) cache->offsets[i] = -1;
	size_t capacity = 0;
	for (int i = 0; i < faces; ++i)
	{
		int face = ui.api.sector_face_id (ui.api.context, sector, i);
		if (face < 0 || face >= faces || seen[face]) continue;
		seen[face] = true;
		int peer = ui.api.sector_face_portal_peer (ui.api.context, sector, face);
		if (peer < 0) continue;
		int n = ui.api.sector_source_face_vertices (ui.api.context, sector, face, NULL, 0);
		if (n < 3 || n > 65536 || cache->count + (size_t)n > 65536) continue;
		vec3_t *v = malloc ((size_t)n * sizeof (*v));
		if (!v) continue;
		if (ui.api.sector_source_face_vertices (ui.api.context, sector, face, v, n) != n) { free (v); continue; }
		double normal[3] = {0,0,0};
		for (int j = 1; j + 1 < n; ++j)
		{
			double a[3], b[3];
			for (int k = 0; k < 3; ++k) { a[k] = (double)v[j][k]-v[0][k]; b[k] = (double)v[j+1][k]-v[0][k]; }
			normal[0] += a[1]*b[2]-a[2]*b[1]; normal[1] += a[2]*b[0]-a[0]*b[2]; normal[2] += a[0]*b[1]-a[1]*b[0];
		}
		double length = sqrt (normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
		if (length < 1e-10) { free (v); continue; }
		for (int k = 0; k < 3; ++k) normal[k] /= length;
		if (cache->count + (size_t)n > capacity)
		{
			size_t next = q_max (capacity ? capacity * 2 : 32, cache->count + (size_t)n);
			if (next > 65536) next = 65536;
			void *p = realloc (cache->edges, next * sizeof (*cache->edges));
			if (!p) { free (v); break; }
			cache->edges = p; capacity = next;
		}
		cache->offsets[face] = (int)cache->count;
		for (int j = 0; j < n; ++j)
		{
			editor_portal_edge_t *edge = &cache->edges[cache->count++];
			edge->face = face; edge->vertex = j; edge->peer = peer;
			edge->ranges = NULL; edge->nranges = 0; edge->resolved = false;
			VectorCopy (v[j], edge->a); VectorCopy (v[(j+1)%n], edge->b);
			memcpy (edge->normal, normal, sizeof (normal));
		}
		free (v);
	}
	free (seen);
	if (!cache->count) return cache;
	cache->order = malloc (cache->count * sizeof (*cache->order));
	if (!cache->order) return cache;
	for (size_t i = 0; i < cache->count; ++i) cache->order[i] = &cache->edges[i];
	qsort (cache->order, cache->count, sizeof (*cache->order), EditorUI_PortalEdgeOrder);
	return cache;
}
static int EditorUI_RangeOrder (const void *a, const void *b)
{
	const editor_edge_range_t *x = a, *y = b;
	return (x->first > y->first) - (x->first < y->first);
}
static int EditorUI_CachePortalRanges (editor_portal_edge_t *edge, editor_edge_range_t *ranges, int count)
{
	if (count && !(count == 1 && ranges[0].first == 0 && ranges[0].last == 1))
	{
		edge->ranges = malloc ((size_t)count * sizeof (*edge->ranges));
		if (!edge->ranges) return count;
		memcpy (edge->ranges, ranges, (size_t)count * sizeof (*ranges));
	}
	edge->nranges = count; edge->resolved = true; return count;
}
/* Return the parts of an edge which are not shared by another coplanar
 * portal face leading to the same sector. Partial overlaps are split rather
 * than hiding the entire edge. */
static int EditorUI_PortalEdgeRanges (int sector, int face, int vertex, editor_edge_range_t out[129])
{
	out[0] = (editor_edge_range_t){0,1};
	editor_portal_sector_t *cache = EditorUI_PortalEdges (sector);
	if (!cache || !cache->order || !cache->offsets || face < 0 || face >= cache->faces ||
		cache->offsets[face] < 0) return 1;
	size_t index = (size_t)cache->offsets[face] + (size_t)vertex;
	if (index >= cache->count || cache->edges[index].face != face || cache->edges[index].vertex != vertex) return 1;
	editor_portal_edge_t *edge = &cache->edges[index];
	if (edge->resolved)
	{
		if (edge->ranges) memcpy (out, edge->ranges, (size_t)edge->nranges * sizeof (*out));
		return edge->nranges;
	}
	size_t low = 0, high = cache->count;
	while (low < high) { size_t m=(low+high)/2; if (cache->order[m]->peer < edge->peer) low=m+1; else high=m; }
	size_t first = low; high = cache->count;
	while (low < high) { size_t m=(low+high)/2; if (cache->order[m]->peer <= edge->peer) low=m+1; else high=m; }
	if (low-first > 512) return EditorUI_CachePortalRanges (edge, out, 1); /* bounded fallback */
	double d[3], len = 0, scale = 1;
	for (int k=0;k<3;++k) { d[k]=(double)edge->b[k]-edge->a[k]; len+=d[k]*d[k]; scale=fmax(scale,fmax(fabs(edge->a[k]),fabs(edge->b[k]))); }
	if (len < 1e-12) return EditorUI_CachePortalRanges (edge, out, 1);
	/* Source coordinates are converted to floats for the viewport. Allow
	 * roughly half a float ulp of projection error without treating nearby
	 * distinct edges as the same line at large world coordinates. */
	double epsilon = 1e-5 + 0.55 * FLT_EPSILON * scale;
	editor_edge_range_t covered[128]; int ncovered = 0;
	for (size_t i=first;i<low;++i)
	{
		const editor_portal_edge_t *other=cache->order[i];
		if (other->face==face) continue;
		double dot=0, dir=0, t[2], residual[2]={0,0}; const float *points[2]={other->a,other->b};
		for (int k=0;k<3;++k) { dot+=edge->normal[k]*other->normal[k]; dir+=d[k]*((double)other->b[k]-other->a[k]); }
		/* Keep even shallow real folds visible. Normals are derived from the
		 * float viewport vertices, so allow a small angular rounding margin. */
		if (dot < 1.0 - 1e-9 || dir >= 0) continue;
		for (int p=0;p<2;++p)
		{
			double projection=0; for (int k=0;k<3;++k) projection+=((double)points[p][k]-edge->a[k])*d[k];
			t[p]=projection/len;
			for (int k=0;k<3;++k) { double x=(double)points[p][k]-edge->a[k]-t[p]*d[k]; residual[p]+=x*x; }
		}
		if (residual[0]>epsilon*epsilon || residual[1]>epsilon*epsilon) continue;
		double a=fmax(0,fmin(t[0],t[1])), b=fmin(1,fmax(t[0],t[1]));
		if (b-a<=1e-7) continue;
		if (ncovered==128) return EditorUI_CachePortalRanges (edge, out, 1);
		covered[ncovered++]=(editor_edge_range_t){a,b};
	}
	if (!ncovered) return EditorUI_CachePortalRanges (edge, out, 1);
	qsort(covered,(size_t)ncovered,sizeof(*covered),EditorUI_RangeOrder);
	int n=0; double cursor=0;
	for (int i=0;i<ncovered;++i)
	{
		if (covered[i].first>cursor+1e-7) out[n++]=(editor_edge_range_t){cursor,covered[i].first};
		cursor=fmax(cursor,covered[i].last);
	}
	if (cursor<1-1e-7) out[n++]=(editor_edge_range_t){cursor,1};
	return EditorUI_CachePortalRanges (edge, out, n);
}
static void EditorUI_DrawSectorTop (cb_context_t *cbx, int sector, qboolean selected)
{
	int h = EditorUI_HAxis (), v = EditorUI_VAxis ();
	if (!ui.api.brush_face_count || !ui.api.brush_face_vertices) return;
	int faces = ui.api.brush_face_count (ui.api.context, sector);
	qboolean *seen = ui.api.sector_face_id && ui.api.sector_source_face_vertices && faces > 0 ? calloc ((size_t)faces, sizeof (*seen)) : NULL;
	for (int face = 0; face < faces; ++face)
	{
		vec3_t fallback[EDITOR3D_MAX_FACE], *verts = fallback;
		int n = 0, source = -1;
		if (seen)
		{
			source = ui.api.sector_face_id (ui.api.context, sector, face);
			if (source >= 0 && source < faces)
			{
				if (seen[source]) continue;
				seen[source] = true;
				n = ui.api.sector_source_face_vertices (ui.api.context, sector, source, NULL, 0);
				if (n >= 3 && n <= 65536) verts = malloc ((size_t)n * sizeof (*verts));
				if (verts && verts != fallback && ui.api.sector_source_face_vertices (ui.api.context, sector, source, verts, n) != n)
				{ free (verts); verts = NULL; }
			}
		}
		if (verts == fallback) n = ui.api.brush_face_vertices (ui.api.context, sector, face, verts, EDITOR3D_MAX_FACE);
		if (!verts || n < 3 || (verts == fallback && n > EDITOR3D_MAX_FACE)) continue;
		if (ui.drag == EDIT_DRAG_MOVE && sector == ui.drag_index)
			for (int i = 0; i < n; ++i) for (int axis = 0; axis < 3; ++axis)
				verts[i][axis] += ui.preview_mins[axis] - ui.original_mins[axis];
		if (ui.drag == EDIT_DRAG_RESIZE && sector == ui.drag_index)
			for (int i = 0; i < n; ++i) for (int axis = 0; axis < 3; ++axis)
			{
				float size = ui.original_maxs[axis] - ui.original_mins[axis];
				if (size > 0.0001f)
					verts[i][axis] = ui.preview_mins[axis] + (verts[i][axis] - ui.original_mins[axis]) *
						(ui.preview_maxs[axis] - ui.preview_mins[axis]) / size;
			}
		if (ui.drag == EDIT_DRAG_VERTEX)
			for (int i = 0; i < n; ++i)
				if (EditorUI_FaceVertexMovesWithDrag (sector, verts == fallback ? face : source, i, verts == fallback, verts[i]))
					VectorCopy (ui.vertex_preview, verts[i]);
		for (int i = 0; i < n; ++i)
		{
			editor_edge_range_t ranges[129] = {{0,1}};
			qboolean explicit_face = selected && (ui.sector_mode == EDIT_SECTOR_FACE || ui.sector_mode == EDIT_SECTOR_KNIFE) && source == ui.sector_face;
			qboolean deforming = ui.drag == EDIT_DRAG_VERTEX || (sector == ui.drag_index && (ui.drag == EDIT_DRAG_FACE || ui.drag == EDIT_DRAG_RESIZE));
			int nranges = !explicit_face && !deforming && verts != fallback && source >= 0 ?
				EditorUI_PortalEdgeRanges (sector, source, i, ranges) : 1;
			for (int r = 0; r < nranges; ++r)
			{
				float x0, y0, x1, y1;
				const float *a = verts[i], *b = verts[(i + 1) % n];
				EditorUI_WorldToScreen (a[h] + (b[h]-a[h])*ranges[r].first, a[v] + (b[v]-a[v])*ranges[r].first, &x0, &y0);
				EditorUI_WorldToScreen (a[h] + (b[h]-a[h])*ranges[r].last, a[v] + (b[v]-a[v])*ranges[r].last, &x1, &y1);
				EditorUI_DrawTopLine (cbx, x0, y0, x1, y1, selected ? 111 : 152);
			}
		}
		if (verts != fallback) free (verts);
	}
	free (seen);
}
static void EditorUI_DrawModels (cb_context_t *cbx)
{
	if (!ui.api.point_count || !ui.api.point_model) return;
	editor_model_view_t view;
	if (ui.perspective)
	{
		vec3_t forward, right, up; Editor3D_Basis (forward, right, up);
		if (!EditorModelPreview_Perspective (&view, glwidth, glheight, (int)EditorUI_Top (), (int)EditorUI_Bottom (),
			ui.camera, forward, right, up, Editor3D_Focal ())) return;
	}
	else if (!EditorModelPreview_Ortho (&view, glwidth, glheight, (int)EditorUI_Top (), (int)EditorUI_Bottom (),
		ui.center, EditorUI_HAxis (), EditorUI_VAxis (), EditorUI_DepthAxis (), ui.zoom)) return;
	int points = ui.api.point_count (ui.api.context);
	for (int i = 0; i < points; ++i)
	{
		editor_map_model_t placement;
		if (!ui.api.point_model (ui.api.context, i, &placement)) continue;
		if (ui.drag == EDIT_DRAG_POINT && ui.drag_index == i)
			for (unsigned a = 0; a < 3; ++a) placement.origin[a] = ui.point_preview[a];
		EditorModelPreview_Draw (cbx, &placement, &view);
	}
}

void EditorUI_Draw (cb_context_t *cbx)
{
	if (!ui.active) return;
	EditorUI_PreparePortalEdges ();
	if (ui.perspective) { Editor3D_Draw (cbx); EditorControls_Draw (cbx); return; }
	const int h = EditorUI_HAxis (), v = EditorUI_VAxis (), depth = EditorUI_DepthAxis ();
	GL_SetCanvas (cbx, CANVAS_DEFAULT);
	R_BindGraphicsPipeline (cbx, PIPELINE_BASIC_BLEND);
	Draw_Fill (cbx, 0, 0, glwidth, glheight, 0, 1);
	const float top = EditorUI_Top (), bottom = EditorUI_Bottom ();
	EditorUI_FillClipped (cbx, 0, top, glwidth, bottom - top, 8, 1);
	Draw_EditorClearDepth (cbx);
	float step = EDITOR_GRID;
	while (step * ui.zoom < 12) step *= 2;
	float left, min_y, right, max_y;
	EditorUI_ScreenToWorld (0, bottom, &left, &min_y);
	EditorUI_ScreenToWorld (glwidth, top, &right, &max_y);
	for (float x = floorf (left / step) * step; x <= right; x += step)
	{
		float sx; EditorUI_WorldToScreen (x, 0, &sx, NULL);
		EditorUI_FillClipped (cbx, floorf (sx), top, x == 0 ? 2 : 1, bottom - top, x == 0 ? 112 : 15, x == 0 ? 0.75f : 0.28f);
	}
	for (float y = floorf (min_y / step) * step; y <= max_y; y += step)
	{
		float sy; EditorUI_WorldToScreen (0, y, NULL, &sy);
		EditorUI_FillClipped (cbx, 0, floorf (sy), glwidth, y == 0 ? 2 : 1, y == 0 ? 112 : 15, y == 0 ? 0.75f : 0.28f);
	}
	EditorUI_DrawModels (cbx);
	int selected = EditorUI_Selected ();
	int count = ui.api.brush_count && ui.api.brush_bounds ? ui.api.brush_count (ui.api.context) : 0;
	for (int i = 0; i < count; ++i)
	{
		if (EditorUI_SectorMode ())
		{
			EditorUI_DrawSectorTop (cbx, i, i == selected);
			if (i == selected && ui.sector_mode == EDIT_SECTOR_VOLUME)
			{
				vec3_t mins, maxs; float x, y;
				if (i == ui.drag_index && (ui.drag == EDIT_DRAG_MOVE || ui.drag == EDIT_DRAG_RESIZE))
				{ VectorCopy (ui.preview_mins, mins); VectorCopy (ui.preview_maxs, maxs); }
				else if (!ui.api.brush_bounds (ui.api.context, i, mins, maxs)) continue;
				EditorUI_WorldToScreen (maxs[h], mins[v], &x, &y);
				EditorUI_FillClipped (cbx, x - 4, y - 4, 8, 8, 111, 1);
			}
			continue;
		}
		vec3_t mins, maxs;
		if (!ui.api.brush_bounds (ui.api.context, i, mins, maxs)) continue;
		if (i == ui.drag_index && (ui.drag == EDIT_DRAG_MOVE || ui.drag == EDIT_DRAG_RESIZE))
		{
			VectorCopy (ui.preview_mins, mins); VectorCopy (ui.preview_maxs, maxs);
		}
		EditorUI_DrawBox (cbx, mins, maxs, i == selected);
	}
	if (ui.drag == EDIT_DRAG_CREATE) EditorUI_DrawBox (cbx, ui.preview_mins, ui.preview_maxs, true);
	if (EditorUI_SectorMode () && ui.sector_mode == EDIT_SECTOR_VERTEX) EditorUI_DrawOrthoVertices (cbx, selected);
	if (ui.api.spawn_origin)
	{
		vec3_t origin;
		if (ui.api.spawn_origin (ui.api.context, origin))
		{
			float x, y; EditorUI_WorldToScreen (origin[h], origin[v], &x, &y);
			EditorUI_FillClipped (cbx, x - 6, y - 1, 13, 3, 111, 1);
			EditorUI_FillClipped (cbx, x - 1, y - 6, 3, 13, 111, 1);
		}
	}
	if (ui.api.point_count && ui.api.point_origin)
	{
		int selected_point = EditorUI_PointSelected (), points = ui.api.point_count (ui.api.context);
		for (int i = 0; i < points; ++i)
		{
			vec3_t origin; if (!ui.api.point_origin (ui.api.context, i, origin)) continue;
			if (ui.drag == EDIT_DRAG_POINT && ui.drag_index == i) VectorCopy (ui.point_preview, origin);
			float x, y; EditorUI_WorldToScreen (origin[h], origin[v], &x, &y);
			int color = i == selected_point ? 15 : 111;
			editor_map_model_t model; qboolean is_model = ui.api.point_model && ui.api.point_model (ui.api.context, i, &model);
			vec3_t corners[8];
			if(is_model && EditorUI_ModelBounds(i,corners))
				for(unsigned corner=0;corner<8;++corner)for(unsigned axis=0;axis<3;++axis)if(!(corner&(1u<<axis)))
				{
					unsigned other=corner|(1u<<axis);float ax,ay,bx,by;
					EditorUI_WorldToScreen(corners[corner][h],corners[corner][v],&ax,&ay);
					EditorUI_WorldToScreen(corners[other][h],corners[other][v],&bx,&by);
					EditorUI_DrawTopLine(cbx,ax,ay,bx,by,color);
				}
			EditorUI_FillClipped (cbx, x - 5, y - 5, 11, 11, color, 1);
			EditorUI_FillClipped (cbx, x - 2, y - 2, 5, 5, is_model ? color : 0, 1);
			if(is_model && x>=0 && x<glwidth-16 && y>=EditorUI_Top()+10 && y<EditorUI_Bottom()-10)
				Draw_String(cbx,x+8,y-8,model.model);
		}
	}
	Draw_Fill (cbx, 0, 0, glwidth, top, 0, 0.9f);
	char title[128]; q_snprintf (title, sizeof (title), "%s  |  %s (%s/%s)  |  TAB: next  1/2/3/4: views",
		EditorUI_SectorMode () ? "SECTOR EDITOR" : "MAP EDITOR", ui.ortho_view == EDIT_ORTHO_TOP ? "TOP" : ui.ortho_view == EDIT_ORTHO_FRONT ? "FRONT" : "RIGHT", EditorUI_AxisName (h), EditorUI_AxisName (v));
	Draw_String (cbx, 8, 5, title);
	Draw_String (cbx, 8, 19, EditorUI_SectorMode () ? "Left: select/move  Shift+drag: box sector  Right: pan  Wheel: zoom" :
		"Left: select/move  Shift+drag: draw  Handle: resize  Right: pan  Wheel: zoom");
	Draw_String (cbx, 8, 31, EditorUI_SectorMode () ? "V: vertex  H: light  G: create  O: type  Ctrl+D: duplicate  [ ]: depth  Del: delete  I: inspector" :
		"H: light  Ctrl+D: duplicate  P: spawn  [ ]: depth  Del: delete  Ctrl+P: play  I: inspector");
	Draw_Fill (cbx, 0, bottom, glwidth, glheight - bottom, 0, 0.9f);
	float mx, my; EditorUI_ScreenToWorld (ui.mouse_x, ui.mouse_y, &mx, &my);
	char line[256];
	if (EditorUI_SectorMode ())
	{
		int type = selected >= 0 && ui.api.sector_prop_type ? ui.api.sector_prop_type (ui.api.context, selected) : -1;
		q_snprintf (line, sizeof (line), "%s %.0f  %s %.0f  FOCUS %s %.0f  GRID %.0f  SECTORS %d  CREATE %s  SELECTED %s",
			EditorUI_AxisName (h), EditorUI_Snap (mx), EditorUI_AxisName (v), EditorUI_Snap (my), EditorUI_AxisName (depth), EditorUI_Snap (ui.center[depth]), EDITOR_GRID, count, EditorUI_PropName (ui.create_prop_type), type < 0 ? "-" : EditorUI_PropName (type));
	}
	else q_snprintf (line, sizeof (line), "%s %.0f  %s %.0f  FOCUS %s %.0f  grid %.0f  zoom %.2f  brushes %d", EditorUI_AxisName (h), EditorUI_Snap (mx), EditorUI_AxisName (v), EditorUI_Snap (my), EditorUI_AxisName (depth), EditorUI_Snap (ui.center[depth]), EDITOR_GRID, ui.zoom, count);
	if (EditorUI_PointSelected () >= 0)
	{
		editor_map_model_t model; qboolean is_model = ui.api.point_model && ui.api.point_model (ui.api.context, EditorUI_PointSelected (), &model);
		q_snprintf (line + strlen (line), sizeof (line) - strlen (line), "  %s %d", is_model ? "MODEL" : "LIGHT", EditorUI_PointSelected ());
	}
	Draw_String (cbx, 8, bottom + 3, line);
	const char *status = EditorUI_StatusText ();
	if (status && *status) Draw_String (cbx, 8, bottom + 17, status);
	EditorControls_Draw (cbx);
}

/* The perspective view clips convex source faces before projection.  Keeping
 * this independent of the compiled BSP lets edits appear immediately. */
#define EDITOR3D_NEAR 1.0f

typedef struct
{
	/* Screen coordinates and reversed, perspective-correct depth (near / z). */
	float xyz[EDITOR3D_MAX_CLIPPED][3];
	int count, color;
	qboolean selected, portal, source_outline;
} editor3d_polygon_t;

static void Editor3D_Basis (vec3_t forward, vec3_t right, vec3_t up)
{
	float cy = cosf (ui.yaw), sy = sinf (ui.yaw), cp = cosf (ui.pitch), sp = sinf (ui.pitch);
	forward[0] = cp * cy; forward[1] = cp * sy; forward[2] = sp;
	/* Match Quake's camera basis: at yaw zero +X is forward and -Y is
	 * screen right. The opposite basis mirrors the entire 3D editor view. */
	right[0] = sy; right[1] = -cy; right[2] = 0;
	up[0] = -sp * cy; up[1] = -sp * sy; up[2] = cp;
}
static float Editor3D_Focal (void) { return q_max (1.0f, (EditorUI_Bottom () - EditorUI_Top ()) * 0.65f); }
static void Editor3D_Ray (float x, float y, vec3_t ray)
{
	vec3_t f, r, u;
	Editor3D_Basis (f, r, u);
	float sx = (x - glwidth * 0.5f) / Editor3D_Focal ();
	float sy = (EditorUI_CenterY () - y) / Editor3D_Focal ();
	for (int i = 0; i < 3; ++i) ray[i] = f[i] + sx * r[i] + sy * u[i];
}
static qboolean Editor3D_RayPlane (const vec3_t ray, const vec3_t point, const vec3_t normal, vec3_t result)
{
	float denominator = DotProduct (ray, normal);
	if (fabsf (denominator) < 0.00001f) return false;
	vec3_t delta;
	VectorSubtract (point, ui.camera, delta);
	float t = DotProduct (delta, normal) / denominator;
	if (t < EDITOR3D_NEAR || t > 1000000.0f) return false;
	VectorMA (ui.camera, t, ray, result);
	return true;
}
static qboolean Editor3D_FaceHit (const vec3_t *v, int count, const vec3_t ray, float *distance, vec3_t hit)
{
	if (count < 3) return false;
	vec3_t a, b, n;
	VectorSubtract (v[1], v[0], a); VectorSubtract (v[2], v[0], b); CrossProduct (a, b, n);
	float len = sqrtf (DotProduct (n, n));
	if (len < 0.0001f) return false;
	VectorScale (n, 1.0f / len, n);
	if (!Editor3D_RayPlane (ray, v[0], n, hit)) return false;
	float sign = 0;
	for (int i = 0; i < count; ++i)
	{
		vec3_t edge, to, cross;
		VectorSubtract (v[(i + 1) % count], v[i], edge); VectorSubtract (hit, v[i], to); CrossProduct (edge, to, cross);
		float side = DotProduct (cross, n);
		if (fabsf (side) < 0.01f) continue;
		if (sign && side * sign < 0) return false;
		sign = side;
	}
	vec3_t delta; VectorSubtract (hit, ui.camera, delta);
	*distance = DotProduct (delta, delta);
	return true;
}
typedef struct { float distance; int sector, face, source_face; vec3_t point; } editor3d_hit_t;
typedef enum { EDIT_PICK_NEAREST, EDIT_PICK_NEXT, EDIT_PICK_CURRENT, EDIT_PICK_OPAQUE } editor3d_pick_t;

static void Editor3D_SaveFaceClick (editor_face_click_t *click, int x, int y, int sector, int face)
{
	click->valid = true;
	click->x = x; click->y = y; click->width = glwidth; click->height = glheight;
	click->sector = sector; click->face = face;
	click->has_revision = ui.api.sector_revision != NULL;
	click->revision = click->has_revision ? ui.api.sector_revision (ui.api.context) : 0;
	VectorCopy (ui.camera, click->camera);
	click->yaw = ui.yaw; click->pitch = ui.pitch;
}
static qboolean Editor3D_SameFaceClickView (const editor_face_click_t *click)
{
	/* Do not reuse a ray after the camera, viewport, document, or mode changes. */
	return click->valid && ui.active && ui.perspective && EditorUI_SectorMode () && ui.sector_mode == EDIT_SECTOR_FACE &&
		click->width == glwidth && click->height == glheight && click->yaw == ui.yaw && click->pitch == ui.pitch &&
		!memcmp (click->camera, ui.camera, sizeof (vec3_t)) &&
		click->has_revision == (ui.api.sector_revision != NULL) &&
		(!click->has_revision || click->revision == ui.api.sector_revision (ui.api.context));
}
static qboolean Editor3D_ShouldCycleFace (int x, int y)
{
	const editor_face_click_t *click = &ui.last_face_click;
	float dx = (float)x - click->x, dy = (float)y - click->y;
	return Editor3D_SameFaceClickView (click) && dx * dx + dy * dy <= EDITOR3D_CLICK_RADIUS * EDITOR3D_CLICK_RADIUS &&
		click->sector == EditorUI_Selected () && click->face == ui.sector_face;
}
static int Editor3D_CompareHits (const void *a, const void *b)
{
	const editor3d_hit_t *x = a, *y = b;
	if (x->distance < y->distance) return -1;
	if (x->distance > y->distance) return 1;
	if (x->sector != y->sector) return x->sector < y->sector ? -1 : 1;
	return (x->source_face > y->source_face) - (x->source_face < y->source_face);
}
static int Editor3D_Pick (float x, float y, vec3_t point, int *display_face, int *source_face, editor3d_pick_t mode)
{
	if (!ui.api.brush_count || !ui.api.brush_face_count || !ui.api.brush_face_vertices) return -1;
	vec3_t ray; Editor3D_Ray (x, y, ray);
	editor3d_hit_t *hits = NULL; size_t nhits = 0, capacity = 0;
	qboolean sectors = EditorUI_SectorMode ();
	int count = ui.api.brush_count (ui.api.context);
	for (int i = 0; i < count; ++i)
	{
		int faces = ui.api.brush_face_count (ui.api.context, i);
		for (int face = 0; face < faces; ++face)
		{
			int source = sectors && ui.api.sector_face_id ? ui.api.sector_face_id (ui.api.context, i, face) : face;
			if (source < 0) continue;
			if (sectors && (ui.sector_mode == EDIT_SECTOR_VOLUME || mode == EDIT_PICK_OPAQUE) && ui.api.sector_face_is_portal &&
				ui.api.sector_face_is_portal (ui.api.context, i, source)) continue;
			vec3_t verts[EDITOR3D_MAX_FACE], hit;
			int n = ui.api.brush_face_vertices (ui.api.context, i, face, verts, EDITOR3D_MAX_FACE);
			float distance;
			if (n >= 3 && n <= EDITOR3D_MAX_FACE && Editor3D_FaceHit (verts, n, ray, &distance, hit))
			{
				size_t at = 0;
				while (at < nhits && (hits[at].sector != i || hits[at].source_face != source)) ++at;
				if (at < nhits && distance >= hits[at].distance) continue;
				if (at == nhits)
				{
					if (nhits == capacity)
					{
						size_t next = capacity ? capacity * 2 : 16;
						editor3d_hit_t *p = realloc (hits, next * sizeof (*p));
						if (!p) { free (hits); return -1; }
						hits = p; capacity = next;
					}
					++nhits;
				}
				hits[at].distance = distance; hits[at].sector = i; hits[at].face = face; hits[at].source_face = source;
				VectorCopy (hit, hits[at].point);
			}
		}
	}
	if (!nhits) { free (hits); return -1; }
	qsort (hits, nhits, sizeof (*hits), Editor3D_CompareHits);
	size_t selected = 0;
	if (mode == EDIT_PICK_NEXT || mode == EDIT_PICK_CURRENT)
		for (size_t i = 0; i < nhits; ++i)
			if (hits[i].sector == EditorUI_Selected () && hits[i].source_face == ui.sector_face)
			{ selected = mode == EDIT_PICK_NEXT ? (i + 1) % nhits : i; break; }
	int result = hits[selected].sector;
	VectorCopy (hits[selected].point, point);
	if (display_face) *display_face = hits[selected].face;
	if (source_face) *source_face = hits[selected].source_face;
	free (hits);
	return result;
}
static qboolean Editor3D_PointVisible (const vec3_t point)
{
	vec3_t forward, right, up, delta, hit; Editor3D_Basis (forward, right, up);
	VectorSubtract (point, ui.camera, delta); float depth = DotProduct (delta, forward);
	if (depth < EDITOR3D_NEAR) return false;
	float sx = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / depth;
	float sy = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / depth;
	if (Editor3D_Pick (sx, sy, hit, NULL, NULL, EDIT_PICK_OPAQUE) < 0) return true;
	VectorSubtract (hit, ui.camera, delta);
	return DotProduct (delta, forward) + 0.01f >= depth;
}
static int Editor3D_PickPoint (int x, int y, vec3_t position)
{
	if (!ui.api.point_count || !ui.api.point_origin) return -1;
	vec3_t forward, right, up; Editor3D_Basis (forward, right, up);
	float best = 12.0f * 12.0f, best_depth = FLT_MAX; int result = -1;
	int count = ui.api.point_count (ui.api.context);
	for (int i = 0; i < count; ++i)
	{
		vec3_t point, delta;
		if (!ui.api.point_origin (ui.api.context, i, point)) continue;
		VectorSubtract (point, ui.camera, delta); float depth = DotProduct (delta, forward);
		if (depth < EDITOR3D_NEAR) continue;
		float sx = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / depth;
		float sy = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / depth;
		float dx = sx - x, dy = sy - y, distance = dx * dx + dy * dy;
		if (distance > best || (distance == best && depth >= best_depth)) continue;
		if (!Editor3D_PointVisible (point)) continue;
		best = distance; best_depth = depth; result = i; VectorCopy (point, position);
	}
	return result;
}
static qboolean Editor3D_PlaceLight (void)
{
	vec3_t ray, point, hit; Editor3D_Ray (ui.mouse_x, ui.mouse_y, ray);
	float length = sqrtf (DotProduct (ray, ray));
	if (length < 0.0001f) return false;
	VectorScale (ray, 1.0f / length, ray);
	int polygon = -1;
	int sector = Editor3D_Pick (ui.mouse_x, ui.mouse_y, hit, &polygon, NULL, EDIT_PICK_OPAQUE);
	vec3_t normal;
	if (sector >= 0 && ui.api.brush_face_vertices)
	{
		vec3_t vertices[EDITOR3D_MAX_FACE], a, b, toward;
		int n = ui.api.brush_face_vertices (ui.api.context, sector, polygon, vertices, EDITOR3D_MAX_FACE);
		if (n < 3) return false;
		VectorSubtract (vertices[1], vertices[0], a); VectorSubtract (vertices[2], vertices[0], b);
		CrossProduct (a, b, normal); length = sqrtf (DotProduct (normal, normal));
		if (length < 0.0001f) return false;
		VectorScale (normal, 1.0f / length, normal); VectorSubtract (ui.camera, hit, toward);
		if (DotProduct (normal, toward) < 0) VectorScale (normal, -1.0f, normal);
		/* Snap can shift a point by up to half a grid step along each axis.
		 * Offset along the surface normal first, then reject candidates hidden
		 * by this or a neighboring wall. */
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			VectorMA (hit, 24.0f + attempt * EDITOR_GRID, normal, point);
			for (int i = 0; i < 3; ++i) point[i] = EditorUI_Snap (point[i]);
			if (Editor3D_PointVisible (point)) return ui.api.point_create_light (ui.api.context, point);
		}
	}
	else
	{
		VectorMA (ui.camera, 96.0f, ray, point);
		for (int i = 0; i < 3; ++i) point[i] = EditorUI_Snap (point[i]);
		if (Editor3D_PointVisible (point)) return ui.api.point_create_light (ui.api.context, point);
	}
	EditorUI_SetStatus ("Could not place a light in front of the visible surface"); return false;
}
/* Snap a face hit to its actual source polygon boundary, not a display
 * triangulation edge. Grid snapping is parameterized along that edge, so it
 * stays on sloped and off-grid geometry as well. */
static qboolean Editor3D_KnifeBoundary (int sector, int face, const vec3_t hit, vec3_t out)
{
	if (!ui.api.sector_source_face_vertices) return false;
	int n = ui.api.sector_source_face_vertices (ui.api.context, sector, face, NULL, 0);
	if (n < 3 || n > 65536) return false;
	vec3_t *v = malloc ((size_t)n * sizeof (*v));
	if (!v) return false;
	if (ui.api.sector_source_face_vertices (ui.api.context, sector, face, v, n) != n) { free (v); return false; }
	float best = FLT_MAX, best_t = 0; int edge = -1;
	for (int i = 0; i < n; ++i)
	{
		vec3_t d, h, delta;
		VectorSubtract (v[(i + 1) % n], v[i], d); VectorSubtract (hit, v[i], h);
		float len = DotProduct (d, d);
		if (len < 0.000001f) continue;
		float t = CLAMP (0.0f, DotProduct (h, d) / len, 1.0f);
		VectorMA (v[i], t, d, delta); VectorSubtract (hit, delta, delta);
		float dist = DotProduct (delta, delta);
		if (dist < best) { best = dist; best_t = t; edge = i; }
	}
	if (edge < 0) { free (v); return false; }
	vec3_t d; VectorSubtract (v[(edge + 1) % n], v[edge], d);
	int axis = 0; for (int i = 1; i < 3; ++i) if (fabsf (d[i]) > fabsf (d[axis])) axis = i;
	float value = v[edge][axis] + best_t * d[axis];
	float snapped = CLAMP (0.0f, (EditorUI_Snap (value) - v[edge][axis]) / d[axis], 1.0f);
	/* An endpoint is also a snap candidate, even on an off-grid edge. */
	if (best_t < fabsf (snapped - best_t)) snapped = 0;
	if (1.0f - best_t < fabsf (snapped - best_t)) snapped = 1;
	VectorMA (v[edge], snapped, d, out);
	free (v); return true;
}
static qboolean Editor3D_KnifePoint (float x, float y, int *sector, int *face, vec3_t point)
{
	vec3_t hit; int polygon = -1, source = -1;
	int found = Editor3D_Pick (x, y, hit, &polygon, &source, EDIT_PICK_CURRENT);
	if (found < 0 || source < 0 || !Editor3D_KnifeBoundary (found, source, hit, point)) return false;
	*sector = found; *face = source; return true;
}
static qboolean Editor3D_Ground (float x, float y, vec3_t point)
{
	vec3_t ray, ground = {0, 0, 0}, up = {0, 0, 1};
	Editor3D_Ray (x, y, ray);
	return Editor3D_RayPlane (ray, ground, up, point);
}
/* Prefer a nearby existing vertex over the grid, including off-grid vertices
 * in other sectors. Limit both screen and world distance so a vertex hidden
 * much farther along the viewing ray cannot unexpectedly win. */
static qboolean Editor3D_SnapVertex (const vec3_t raw, vec3_t snapped)
{
	ui.vertex_snap = false;
	if (!ui.api.brush_count || !ui.api.sector_vertex_count || !ui.api.sector_vertex) return false;
	vec3_t forward, right, up, delta;
	Editor3D_Basis (forward, right, up);
	VectorSubtract (raw, ui.camera, delta);
	float depth = DotProduct (delta, forward);
	if (depth < EDITOR3D_NEAR) return false;
	float raw_x = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / depth;
	float raw_y = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / depth;
	float best = EDITOR3D_VERTEX_SNAP_DISTANCE * EDITOR3D_VERTEX_SNAP_DISTANCE;
	int sectors = ui.api.brush_count (ui.api.context);
	for (int sector = 0; sector < sectors; ++sector)
	{
		vec3_t mins, maxs;
		if (ui.api.brush_bounds && ui.api.brush_bounds (ui.api.context, sector, mins, maxs))
		{
			qboolean near = true;
			for (int i = 0; i < 3; ++i)
				if (raw[i] < mins[i] - EDITOR3D_VERTEX_SNAP_DISTANCE || raw[i] > maxs[i] + EDITOR3D_VERTEX_SNAP_DISTANCE) near = false;
			if (!near) continue;
		}
		int count = ui.api.sector_vertex_count (ui.api.context, sector);
		for (int vertex = 0; vertex < count; ++vertex)
		{
			vec3_t point, offset, source_offset;
			if ((sector == ui.drag_index && vertex == ui.sector_vertex) ||
				!ui.api.sector_vertex (ui.api.context, sector, vertex, point)) continue;
			if (!isfinite (point[0]) || !isfinite (point[1]) || !isfinite (point[2])) continue;
			/* Coincident vertices already move with the source; they are not
			 * a separate snap target when the pointer returns to the origin. */
			if (ui.api.sector_vertex_coincident &&
				ui.api.sector_vertex_coincident (ui.api.context, ui.drag_index, ui.sector_vertex, sector, vertex)) continue;
			VectorSubtract (point, ui.vertex_origin, source_offset);
			/* Distinct document vertices can collapse to the same viewport
			 * float. Do not silently snap to an indistinguishable target. */
			if (ui.api.sector_vertex_coincident && DotProduct (source_offset, source_offset) == 0) continue;
			if (!ui.api.sector_vertex_coincident && DotProduct (source_offset, source_offset) < 0.000001f &&
				(!ui.api.sector_same_vertex || ui.api.sector_same_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, sector, vertex))) continue;
			VectorSubtract (point, raw, offset);
			float distance = DotProduct (offset, offset);
			if (distance >= best) continue;
			VectorSubtract (point, ui.camera, delta);
			float z = DotProduct (delta, forward);
			if (z < EDITOR3D_NEAR) continue;
			float x = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / z;
			float y = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / z;
			float dx = x - raw_x, dy = y - raw_y;
			if (dx * dx + dy * dy > EDITOR3D_VERTEX_SNAP_PIXELS * EDITOR3D_VERTEX_SNAP_PIXELS) continue;
			best = distance;
			VectorCopy (point, snapped);
			ui.vertex_snap_sector = sector; ui.vertex_snap_vertex = vertex;
			ui.vertex_snap = true;
		}
	}
	return ui.vertex_snap;
}
static void Editor3D_UpdateDrag (void)
{
	if (ui.drag == EDIT_DRAG_FACE)
	{
		ui.face_distance = EditorUI_Snap ((ui.start_y - ui.mouse_y) * 0.5f);
		return;
	}
	vec3_t ray, hit;
	Editor3D_Ray (ui.mouse_x, ui.mouse_y, ray);
	if (ui.drag == EDIT_DRAG_POINT && Editor3D_RayPlane (ray, ui.point_original, ui.drag_normal, hit))
	{
		VectorCopy (ui.point_original, ui.point_preview);
		float dx = ui.mouse_x - ui.start_x, dy = ui.mouse_y - ui.start_y;
		if (dx * dx + dy * dy <= EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD) return;
		for (int i = 0; i < 3; ++i)
		{
			float delta = hit[i] - ui.drag_point[i];
			if (fabsf (delta) > 0.0001f) ui.point_preview[i] = EditorUI_Snap (ui.point_original[i] + delta);
		}
	}
	else if (ui.drag == EDIT_DRAG_CREATE)
	{
		if (!Editor3D_Ground (ui.mouse_x, ui.mouse_y, hit)) return;
		hit[0] = EditorUI_Snap (hit[0]); hit[1] = EditorUI_Snap (hit[1]);
		ui.preview_mins[0] = q_min (ui.anchor_x, hit[0]); ui.preview_maxs[0] = q_max (ui.anchor_x, hit[0]);
		ui.preview_mins[1] = q_min (ui.anchor_y, hit[1]); ui.preview_maxs[1] = q_max (ui.anchor_y, hit[1]);
	}
	else if (ui.drag == EDIT_DRAG_MOVE && Editor3D_RayPlane (ray, ui.drag_point, ui.drag_normal, hit))
	{
		float dx = ui.mouse_x - ui.start_x, dy = ui.mouse_y - ui.start_y;
		for (int i = 0; i < 3; ++i)
		{
			float delta = hit[i] - ui.drag_point[i];
			if (dx * dx + dy * dy <= EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD || fabsf (delta) < 0.0001f) delta = 0;
			else delta = EditorUI_Snap (ui.original_mins[i] + delta) - ui.original_mins[i];
			ui.preview_mins[i] = ui.original_mins[i] + delta;
			ui.preview_maxs[i] = ui.original_maxs[i] + delta;
		}
	}
	else if (ui.drag == EDIT_DRAG_VERTEX && Editor3D_RayPlane (ray, ui.vertex_origin, ui.drag_normal, hit))
	{
		float dx = ui.mouse_x - ui.start_x, dy = ui.mouse_y - ui.start_y;
		if (dx * dx + dy * dy > EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD) ui.vertex_dragged = true;
		if (!ui.vertex_dragged)
		{
			VectorCopy (ui.vertex_origin, ui.vertex_preview);
			ui.vertex_snap = false;
			return;
		}
		vec3_t raw;
		for (int i = 0; i < 3; ++i)
			raw[i] = ui.vertex_origin[i] + hit[i] - ui.drag_point[i];
		if (!Editor3D_SnapVertex (raw, ui.vertex_preview))
			for (int i = 0; i < 3; ++i)
				ui.vertex_preview[i] = fabsf (hit[i] - ui.drag_point[i]) > 0.0001f ? EditorUI_Snap (raw[i]) : ui.vertex_origin[i];
	}
}
static void Editor3D_Update (int x, int y)
{
	if (ui.face_press.valid)
	{
		float dx = (float)x - ui.face_press.x, dy = (float)y - ui.face_press.y;
		if (dx * dx + dy * dy > EDITOR3D_DRAG_THRESHOLD * EDITOR3D_DRAG_THRESHOLD)
			ui.face_press_dragged = true;
	}
	if (ui.drag == EDIT_DRAG_PAN)
	{
		if (x != ui.mouse_x || y != ui.mouse_y) EditorUI_ResetFaceCycle ();
		ui.yaw -= (x - ui.mouse_x) * 0.005f;
		ui.pitch = CLAMP (-1.53f, ui.pitch - (y - ui.mouse_y) * 0.005f, 1.53f);
	}
	double dt = ui.last_update > 0 ? realtime - ui.last_update : 0;
	ui.last_update = realtime;
	dt = CLAMP (0.0, dt, 0.1);
	float speed = (ui.shift ? 640.0f : 320.0f) * (float)dt;
	float forward = (float)ui.move_forward - (float)ui.move_back;
	float right = (float)ui.move_right - (float)ui.move_left;
	if (speed != 0 && (forward || right || ui.move_up != ui.move_down)) EditorUI_ResetFaceCycle ();
	ui.camera[0] += speed * (cosf (ui.yaw) * forward + sinf (ui.yaw) * right);
	ui.camera[1] += speed * (sinf (ui.yaw) * forward - cosf (ui.yaw) * right);
	ui.camera[2] += speed * ((float)ui.move_up - (float)ui.move_down);
	ui.mouse_x = x; ui.mouse_y = y;
	if (ui.sector_mode == EDIT_SECTOR_KNIFE)
	{
		int sector = -1, face = -1;
		ui.knife_preview_valid = EditorUI_InViewport (x, y) && Editor3D_KnifePoint (x, y, &sector, &face, ui.knife_preview) &&
			(!ui.knife_pending || (sector == ui.knife_sector && face == ui.knife_face));
	}
	if (ui.drag == EDIT_DRAG_CREATE || ui.drag == EDIT_DRAG_MOVE || ui.drag == EDIT_DRAG_FACE || ui.drag == EDIT_DRAG_VERTEX || ui.drag == EDIT_DRAG_POINT)
		Editor3D_UpdateDrag ();
}
static int Editor3D_NearestVertex (int sector, float mouse_x, float mouse_y, vec3_t position)
{
	if (!ui.api.sector_vertex_count || !ui.api.sector_vertex) return -1;
	vec3_t forward, right, up;
	Editor3D_Basis (forward, right, up);
	int best = -1; float best_dist = 14.0f * 14.0f;
	int count = ui.api.sector_vertex_count (ui.api.context, sector);
	for (int i = 0; i < count; ++i)
	{
		vec3_t vertex, delta;
		if (!ui.api.sector_vertex (ui.api.context, sector, i, vertex)) continue;
		VectorSubtract (vertex, ui.camera, delta);
		float depth = DotProduct (delta, forward);
		if (depth < EDITOR3D_NEAR) continue;
		float sx = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / depth;
		float sy = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / depth;
		float dx = sx - mouse_x, dy = sy - mouse_y, d = dx * dx + dy * dy;
		if (d < best_dist) { best = i; best_dist = d; VectorCopy (vertex, position); }
	}
	return best;
}
static qboolean Editor3D_KeyEvent (int key, qboolean down)
{
	qboolean sectors = EditorUI_SectorMode ();
	if (sectors && !ui.ctrl && down)
	{
		if (key == 'f' || key == 'v' || key == 'k')
		{
			EditorUI_ResetFaceCycle ();
			editor_sector_mode_t mode = key == 'f' ? EDIT_SECTOR_FACE : key == 'v' ? EDIT_SECTOR_VERTEX : EDIT_SECTOR_KNIFE;
			ui.knife_pending = ui.knife_preview_valid = false;
			ui.sector_mode = ui.sector_mode == mode ? EDIT_SECTOR_VOLUME : mode;
			ui.drag = EDIT_DRAG_NONE; if (mode != EDIT_SECTOR_KNIFE) ui.sector_face = -1;
			ui.sector_vertex = -1;
			EditorUI_SetStatus (ui.sector_mode == EDIT_SECTOR_FACE ? "Face mode: click a face, drag to extrude" :
				ui.sector_mode == EDIT_SECTOR_VERTEX ? "Vertex mode: click near a vertex and drag" :
				ui.sector_mode == EDIT_SECTOR_KNIFE ? "Knife: click near a face edge" : "Sector mode");
			return true;
		}
		if (ui.sector_mode == EDIT_SECTOR_FACE && ui.sector_face >= 0 && EditorUI_Selected () >= 0)
		{
			int sector = EditorUI_Selected ();
			if (key == 'l')
			{
				if (ui.portal_sector < 0)
				{
					ui.portal_sector = sector; ui.portal_face = ui.sector_face;
					ui.portal_revision = ui.api.sector_revision ? ui.api.sector_revision (ui.api.context) : 0;
					EditorUI_SetStatus ("Portal source marked; select the matching face and press L");
				}
				else if (ui.api.sector_revision && ui.portal_revision != ui.api.sector_revision (ui.api.context))
				{
					EditorUI_ClearPendingPortal (); EditorUI_SetStatus ("Portal source changed; select it again");
				}
				else if (ui.portal_sector == sector && ui.portal_face == ui.sector_face)
				{
					ui.portal_sector = ui.portal_face = -1; EditorUI_SetStatus ("Portal link cancelled");
				}
				else
				{
					char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
					if (ui.api.sector_link_portal && ui.api.sector_link_portal (ui.api.context, ui.portal_sector, ui.portal_face, sector, ui.sector_face))
					{
						ui.portal_sector = ui.portal_face = ui.sector_face = -1; EditorUI_SetStatus ("Portal linked; select a face to continue");
					}
					else EditorUI_FallbackError (old_status, "Portal link failed; select matching opposite faces");
				}
				return true;
			}
			if (key == 'u')
			{
				char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
				if (ui.api.sector_unlink_portal && ui.api.sector_unlink_portal (ui.api.context, sector, ui.sector_face))
				{
					ui.sector_face = -1; EditorUI_ClearPendingPortal (); EditorUI_SetStatus ("Portal unlinked; select a face to continue");
				}
				else EditorUI_FallbackError (old_status, "Could not unlink portal");
				return true;
			}
			if (key == 't')
			{
				char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
				if (ui.api.sector_apply_material && ui.api.sector_apply_material (ui.api.context, sector, ui.sector_face))
					EditorUI_SetStatus ("Face material updated");
				else EditorUI_FallbackError (old_status, "Could not update face material");
				return true;
			}
		}
	}
	/* Always process movement releases, even when Ctrl was pressed afterwards. */
	if (!ui.ctrl || !down)
	{
		if (key == 'w') { ui.move_forward = down; return true; }
		if (key == 's') { ui.move_back = down; return true; }
		if (key == 'a') { ui.move_left = down; return true; }
		if (key == 'd') { ui.move_right = down; return true; }
		if (key == 'e') { ui.move_up = down; return true; }
		if (key == 'q') { ui.move_down = down; return true; }
	}
	if (key == K_MOUSE2 || key == K_MOUSE3)
	{
		if (down && ui.drag == EDIT_DRAG_NONE && EditorUI_InViewport (ui.mouse_x, ui.mouse_y)) ui.drag = EDIT_DRAG_PAN;
		else if (!down && ui.drag == EDIT_DRAG_PAN) ui.drag = EDIT_DRAG_NONE;
		return true;
	}
	if ((key == K_MWHEELUP || key == K_MWHEELDOWN) && down && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
	{
		EditorUI_ResetFaceCycle ();
		vec3_t f, r, u; Editor3D_Basis (f, r, u);
		VectorMA (ui.camera, key == K_MWHEELUP ? 32 : -32, f, ui.camera);
		return true;
	}
	if (key == K_MOUSE1)
	{
		if (down && ui.drag == EDIT_DRAG_NONE && !ui.shift && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
		{
			vec3_t position; int point = Editor3D_PickPoint (ui.mouse_x, ui.mouse_y, position);
			if (point >= 0 && ui.api.point_select)
			{
				ui.api.point_select (ui.api.context, point); EditorUI_ClearPendingPortal (); EditorUI_ResetFaceCycle ();
				ui.knife_pending = ui.knife_preview_valid = false; ui.sector_face = ui.sector_vertex = -1;
				vec3_t ray, right, up; Editor3D_Basis (ui.drag_normal, right, up); Editor3D_Ray (ui.mouse_x, ui.mouse_y, ray);
				if (Editor3D_RayPlane (ray, position, ui.drag_normal, ui.drag_point))
				{
					ui.drag_index = point; ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y;
					VectorCopy (position, ui.point_original); VectorCopy (position, ui.point_preview);
					ui.point_revision = ui.api.sector_revision ? ui.api.sector_revision (ui.api.context) : 0;
					ui.drag = EDIT_DRAG_POINT;
				}
				return true;
			}
		}
		if (sectors && ui.sector_mode == EDIT_SECTOR_KNIFE)
		{
			if (down && ui.drag == EDIT_DRAG_NONE && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
			{
				int sector = -1, face = -1; vec3_t point;
				if (!Editor3D_KnifePoint (ui.mouse_x, ui.mouse_y, &sector, &face, point))
					EditorUI_SetStatus ("Knife: click near a face edge");
				else if (!ui.knife_pending)
				{
					if (ui.api.select) ui.api.select (ui.api.context, sector);
					ui.sector_face = ui.knife_face = face; ui.knife_sector = sector;
					ui.knife_revision = ui.api.sector_revision ? ui.api.sector_revision (ui.api.context) : 0;
					VectorCopy (point, ui.knife_start); ui.knife_pending = true; ui.knife_preview_valid = false;
					EditorUI_SetStatus ("Knife: select the second edge");
				}
				else if (sector != ui.knife_sector || face != ui.knife_face)
					EditorUI_SetStatus ("Knife: choose the second edge on the same face");
				else
				{
					char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
					if (ui.api.sector_knife && ui.api.sector_knife (ui.api.context, sector, face, ui.knife_start, point))
					{
						ui.knife_pending = ui.knife_preview_valid = false; ui.sector_face = -1;
						EditorUI_ClearPendingPortal (); EditorUI_SetStatus ("Knife cut complete");
					}
					else EditorUI_FallbackError (old_status, "Knife cut rejected; choose two distinct edges");
				}
			}
			return true;
		}
		if (down && ui.drag == EDIT_DRAG_NONE && !EditorUI_InViewport (ui.mouse_x, ui.mouse_y)) EditorUI_ResetFaceCycle ();
		if (down && ui.drag == EDIT_DRAG_NONE && EditorUI_InViewport (ui.mouse_x, ui.mouse_y))
		{
			vec3_t point;
			if (ui.shift)
			{
				EditorUI_ResetFaceCycle ();
				if (!Editor3D_Ground (ui.mouse_x, ui.mouse_y, point)) { EditorUI_SetStatus ("Aim toward the ground plane to create a brush"); return true; }
				ui.anchor_x = EditorUI_Snap (point[0]); ui.anchor_y = EditorUI_Snap (point[1]);
				ui.preview_mins[0] = ui.anchor_x; ui.preview_mins[1] = ui.anchor_y; ui.preview_mins[2] = 0;
				ui.preview_maxs[0] = ui.anchor_x; ui.preview_maxs[1] = ui.anchor_y; ui.preview_maxs[2] = sectors ? 128 : 64;
				ui.drag = EDIT_DRAG_CREATE;
			}
			else
			{
				int polygon = -1, face = -1;
				qboolean face_mode = sectors && ui.sector_mode == EDIT_SECTOR_FACE;
				qboolean ctrl_cycle = face_mode && ui.ctrl;
				qboolean repeat = face_mode && !ui.ctrl && Editor3D_ShouldCycleFace (ui.mouse_x, ui.mouse_y);
				int previous_sector = EditorUI_Selected (), previous_face = ui.sector_face;
				/* Preserve the current hit on press so a drag still moves the
				 * face the user saw. Advance a repeated click only on release. */
				editor3d_pick_t pick_mode = ctrl_cycle ? EDIT_PICK_NEXT : repeat ? EDIT_PICK_CURRENT : EDIT_PICK_NEAREST;
				int selected = Editor3D_Pick (ui.mouse_x, ui.mouse_y, point, &polygon, &face, pick_mode);
				if (face_mode && selected >= 0)
				{
					Editor3D_SaveFaceClick (&ui.face_press, ui.mouse_x, ui.mouse_y, selected, face);
					ui.face_press_dragged = false;
					ui.face_press_cycle = repeat && selected == previous_sector && face == previous_face;
				}
				else EditorUI_ResetFaceCycle ();
				if (sectors && ui.sector_mode == EDIT_SECTOR_VERTEX && selected < 0) selected = EditorUI_Selected ();
				if (ui.api.select) ui.api.select (ui.api.context, selected);
				ui.drag_index = selected;
				if (selected >= 0 && sectors && ui.sector_mode == EDIT_SECTOR_FACE)
				{
					ui.sector_face = face; ui.sector_vertex = -1;
					if (ui.api.brush_face_vertices)
					{
						vec3_t verts[EDITOR3D_MAX_FACE], a, b;
						int n = ui.api.brush_face_vertices (ui.api.context, selected, polygon, verts, EDITOR3D_MAX_FACE);
						if (n >= 3)
						{
							VectorSubtract (verts[1], verts[0], a); VectorSubtract (verts[2], verts[0], b);
							CrossProduct (a, b, ui.face_normal);
							float length = sqrtf (DotProduct (ui.face_normal, ui.face_normal));
							if (length > 0.0001f)
							{
								VectorScale (ui.face_normal, 1.0f / length, ui.face_normal);
								if (!ctrl_cycle) { ui.start_y = ui.mouse_y; ui.face_distance = 0; ui.drag = EDIT_DRAG_FACE; }
							}
						}
					}
				}
				else if (selected >= 0 && sectors && ui.sector_mode == EDIT_SECTOR_VERTEX)
				{
					ui.sector_vertex = Editor3D_NearestVertex (selected, ui.mouse_x, ui.mouse_y, ui.vertex_origin);
					ui.sector_face = -1;
					if (ui.sector_vertex >= 0)
					{
						vec3_t ray, right, up;
						Editor3D_Basis (ui.drag_normal, right, up); Editor3D_Ray (ui.mouse_x, ui.mouse_y, ray);
						if (Editor3D_RayPlane (ray, ui.vertex_origin, ui.drag_normal, ui.drag_point))
						{
							VectorCopy (ui.vertex_origin, ui.vertex_preview); ui.drag = EDIT_DRAG_VERTEX;
							ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y; ui.vertex_snap = ui.vertex_dragged = false;
						}
					}
					else EditorUI_SetStatus ("Click near a visible vertex");
				}
				else if (selected >= 0 && ui.api.brush_bounds && ui.api.brush_bounds (ui.api.context, selected, ui.original_mins, ui.original_maxs))
				{
					vec3_t right, up; Editor3D_Basis (ui.drag_normal, right, up); VectorCopy (point, ui.drag_point);
					VectorCopy (ui.original_mins, ui.preview_mins); VectorCopy (ui.original_maxs, ui.preview_maxs);
					ui.start_x = ui.mouse_x; ui.start_y = ui.mouse_y;
					ui.drag = EDIT_DRAG_MOVE;
				}
			}
		}
		else if (!down && (ui.drag == EDIT_DRAG_CREATE || ui.drag == EDIT_DRAG_MOVE || ui.drag == EDIT_DRAG_FACE || ui.drag == EDIT_DRAG_VERTEX || ui.drag == EDIT_DRAG_POINT))
		{
			Editor3D_UpdateDrag ();
			if (ui.drag == EDIT_DRAG_POINT && ui.api.point_set_origin && memcmp (ui.point_original, ui.point_preview, sizeof (vec3_t)))
				ui.api.point_set_origin (ui.api.context, ui.drag_index, ui.point_preview);
			else if (ui.drag == EDIT_DRAG_CREATE && ui.preview_maxs[0] > ui.preview_mins[0] && ui.preview_maxs[1] > ui.preview_mins[1])
				EditorUI_Create (ui.preview_mins, ui.preview_maxs);
			else if (ui.drag == EDIT_DRAG_MOVE && ui.api.set_bounds &&
				(memcmp (ui.preview_mins, ui.original_mins, sizeof (vec3_t)) || memcmp (ui.preview_maxs, ui.original_maxs, sizeof (vec3_t))))
				ui.api.set_bounds (ui.api.context, ui.drag_index, ui.preview_mins, ui.preview_maxs);
			else if (ui.drag == EDIT_DRAG_FACE && ui.face_distance != 0 && ui.api.sector_move_face)
			{
				char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
				vec3_t delta; VectorScale (ui.face_normal, ui.face_distance, delta);
				if (!ui.api.sector_move_face (ui.api.context, ui.drag_index, ui.sector_face, delta))
					EditorUI_FallbackError (old_status, "Face move rejected; it must preserve valid sectors and portals");
			}
			else if (ui.drag == EDIT_DRAG_VERTEX && (memcmp (ui.vertex_origin, ui.vertex_preview, sizeof (vec3_t)) || (ui.vertex_snap && ui.api.sector_merge_vertex)) && (ui.api.sector_set_vertex || (ui.vertex_snap && ui.api.sector_merge_vertex)))
			{
				char old_status[256]; q_strlcpy (old_status, EditorUI_SessionStatus (), sizeof (old_status));
				qboolean ok = ui.vertex_snap && ui.api.sector_merge_vertex ?
					ui.api.sector_merge_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, ui.vertex_snap_sector, ui.vertex_snap_vertex) :
					ui.api.sector_set_vertex (ui.api.context, ui.drag_index, ui.sector_vertex, ui.vertex_preview);
				if (!ok)
					EditorUI_FallbackError (old_status, "Vertex move rejected; it must preserve valid sectors and portals");
				else
				{
					ui.sector_face = ui.sector_vertex = -1;
					EditorUI_ClearPendingPortal ();
				}
			}
			ui.drag = EDIT_DRAG_NONE;
			ui.vertex_snap = false;
		}
		if (!down && ui.face_press.valid)
		{
			qboolean click = !ui.face_press_dragged && EditorUI_InViewport (ui.mouse_x, ui.mouse_y) && Editor3D_SameFaceClickView (&ui.face_press) &&
				ui.face_press.sector == EditorUI_Selected () && ui.face_press.face == ui.sector_face;
			if (click && ui.face_press_cycle)
			{
				vec3_t point; int polygon = -1, face = -1;
				int sector = Editor3D_Pick (ui.mouse_x, ui.mouse_y, point, &polygon, &face, EDIT_PICK_NEXT);
				if (sector >= 0)
				{
					if (ui.api.select) ui.api.select (ui.api.context, sector);
					ui.sector_face = face;
				}
				else click = false;
			}
			if (click) Editor3D_SaveFaceClick (&ui.last_face_click, ui.mouse_x, ui.mouse_y, EditorUI_Selected (), ui.sector_face);
			else ui.last_face_click.valid = false;
			ui.face_press.valid = false;
		}
		return true;
	}
	if (!down) return true;
	if (ui.ctrl && ui.api.action)
	{
		if (key == 's') ui.api.action (ui.api.context, EDITOR_UI_SAVE);
		else if (key == 'z') { EditorUI_ClearPendingPortal (); ui.api.action (ui.api.context, ui.shift ? EDITOR_UI_REDO : EDITOR_UI_UNDO); }
		else if (key == 'y') { EditorUI_ClearPendingPortal (); ui.api.action (ui.api.context, EDITOR_UI_REDO); }
		else if (key == 'p') ui.api.action (ui.api.context, EDITOR_UI_PLAY);
		return true;
	}
	if (key == K_DEL && ui.api.remove_selected)
	{
		EditorUI_ClearPendingPortal (); ui.sector_face = ui.sector_vertex = -1; ui.api.remove_selected (ui.api.context);
	}
	else if (key == 'p' && ui.api.place_spawn)
	{
		vec3_t p; if (Editor3D_Ground (ui.mouse_x, ui.mouse_y, p))
		{ p[0] = EditorUI_Snap (p[0]); p[1] = EditorUI_Snap (p[1]); p[2] = 32; ui.api.place_spawn (ui.api.context, p); }
	}
	else if ((key == '[' || key == ']') && ui.api.brush_bounds && ui.api.set_bounds)
	{
		int selected = EditorUI_Selected (); vec3_t mins, maxs;
		if (selected >= 0 && ui.api.brush_bounds (ui.api.context, selected, mins, maxs))
		{
			float delta = key == ']' ? EDITOR_GRID : -EDITOR_GRID;
			if (ui.shift) maxs[2] = q_max (mins[2] + EDITOR_GRID, maxs[2] + delta);
			else { mins[2] += delta; maxs[2] += delta; }
			ui.api.set_bounds (ui.api.context, selected, mins, maxs);
		}
	}
	return true;
}
static int Editor3D_Clip2D (float input[][3], int count, float output[][3], int axis, float edge, qboolean greater)
{
	int out = 0;
	for (int i = 0; i < count; ++i)
	{
		float *a = input[i], *b = input[(i + 1) % count];
		qboolean inside_a = greater ? a[axis] >= edge : a[axis] <= edge;
		qboolean inside_b = greater ? b[axis] >= edge : b[axis] <= edge;
		if (inside_a && out < EDITOR3D_MAX_CLIPPED) { memcpy (output[out++], a, sizeof (output[0])); }
		if (inside_a != inside_b && out < EDITOR3D_MAX_CLIPPED)
		{
			float t = (edge - a[axis]) / (b[axis] - a[axis]);
			for (int component = 0; component < 3; ++component)
				output[out][component] = a[component] + t * (b[component] - a[component]);
			++out;
		}
	}
	return out;
}
static qboolean Editor3D_ProjectFace (const vec3_t *points, int count, qboolean selected, editor3d_polygon_t *poly)
{
	if (count < 3 || count > EDITOR3D_MAX_FACE) return false;
	vec3_t f, r, u, view[EDITOR3D_MAX_CLIPPED], clipped[EDITOR3D_MAX_CLIPPED];
	Editor3D_Basis (f, r, u);
	for (int i = 0; i < count; ++i)
	{
		vec3_t delta; VectorSubtract (points[i], ui.camera, delta);
		view[i][0] = DotProduct (delta, r); view[i][1] = DotProduct (delta, u); view[i][2] = DotProduct (delta, f);
	}
	int n = 0;
	for (int i = 0; i < count; ++i)
	{
		float *a = view[i], *b = view[(i + 1) % count];
		qboolean inside_a = a[2] >= EDITOR3D_NEAR, inside_b = b[2] >= EDITOR3D_NEAR;
		if (inside_a && n < EDITOR3D_MAX_CLIPPED) { VectorCopy (a, clipped[n]); ++n; }
		if (inside_a != inside_b && n < EDITOR3D_MAX_CLIPPED)
		{
			float t = (EDITOR3D_NEAR - a[2]) / (b[2] - a[2]);
			for (int j = 0; j < 3; ++j) clipped[n][j] = a[j] + t * (b[j] - a[j]);
			++n;
		}
	}
	if (n < 3) return false;
	float a2[EDITOR3D_MAX_CLIPPED][3], b2[EDITOR3D_MAX_CLIPPED][3];
	for (int i = 0; i < n; ++i)
	{
		a2[i][0] = glwidth * 0.5f + clipped[i][0] * Editor3D_Focal () / clipped[i][2];
		a2[i][1] = EditorUI_CenterY () - clipped[i][1] * Editor3D_Focal () / clipped[i][2];
		/* Screen-space interpolation of reciprocal view depth is linear. */
		a2[i][2] = EDITOR3D_NEAR / clipped[i][2];
	}
	n = Editor3D_Clip2D (a2, n, b2, 0, 0, true); if (n < 3) return false;
	n = Editor3D_Clip2D (b2, n, a2, 0, glwidth, false); if (n < 3) return false;
	n = Editor3D_Clip2D (a2, n, b2, 1, EditorUI_Top (), true); if (n < 3) return false;
	n = Editor3D_Clip2D (b2, n, a2, 1, EditorUI_Bottom (), false); if (n < 3) return false;
	vec3_t e1, e2, normal;
	VectorSubtract (points[1], points[0], e1); VectorSubtract (points[2], points[0], e2); CrossProduct (e1, e2, normal);
	float len = sqrtf (DotProduct (normal, normal));
	float light = len > 0.0001f ? fabsf ((normal[0] * 0.35f + normal[1] * 0.45f + normal[2] * 0.82f) / len) : 0.5f;
	poly->color = light > 0.7f ? 106 : light > 0.4f ? 102 : 98;
	poly->count = n; poly->selected = selected;
	memcpy (poly->xyz, a2, n * sizeof (a2[0]));
	return true;
}
static void Editor3D_Line (cb_context_t *cbx, const float a[3], const float b[3], int color)
{
	float dx = b[0] - a[0], dy = b[1] - a[1], len = sqrtf (dx * dx + dy * dy);
	if (len < 0.01f) return;
	float nx = -dy / len, ny = dx / len;
	/* Move lines a tiny relative amount toward the camera to avoid z-fighting
	 * with their own face. Their depth test still hides occluded edges. */
	float za = q_min (1.0f, a[2] * 1.00001f), zb = q_min (1.0f, b[2] * 1.00001f);
	float v[4][3] = {{a[0] + nx, a[1] + ny, za}, {b[0] + nx, b[1] + ny, zb},
		{b[0] - nx, b[1] - ny, zb}, {a[0] - nx, a[1] - ny, za}};
	Draw_EditorPolygonDepth (cbx, (const float (*)[3])v, 4, color, 1, false);
}
/* Clip a true source edge before projection. Unlike outlining the clipped
 * render triangles, this never adds triangulation or viewport-clip edges. */
static qboolean Editor3D_ProjectEdge (const vec3_t start, const vec3_t end, float out[2][3])
{
	vec3_t f, r, u, view[2]; const float *points[2] = {start, end};
	Editor3D_Basis (f, r, u);
	for (int i = 0; i < 2; ++i)
	{
		vec3_t delta; VectorSubtract (points[i], ui.camera, delta);
		view[i][0] = DotProduct (delta, r); view[i][1] = DotProduct (delta, u); view[i][2] = DotProduct (delta, f);
	}
	if (view[0][2] < EDITOR3D_NEAR && view[1][2] < EDITOR3D_NEAR) return false;
	if (view[0][2] < EDITOR3D_NEAR || view[1][2] < EDITOR3D_NEAR)
	{
		int behind = view[0][2] < EDITOR3D_NEAR ? 0 : 1, front = 1 - behind;
		float t = (EDITOR3D_NEAR - view[behind][2]) / (view[front][2] - view[behind][2]);
		for (int i = 0; i < 3; ++i) view[behind][i] += t * (view[front][i] - view[behind][i]);
	}
	for (int i = 0; i < 2; ++i)
	{
		out[i][0] = glwidth * 0.5f + view[i][0] * Editor3D_Focal () / view[i][2];
		out[i][1] = EditorUI_CenterY () - view[i][1] * Editor3D_Focal () / view[i][2];
		out[i][2] = q_min (1.0f, EDITOR3D_NEAR / view[i][2]);
	}
	/* Clip in screen space, including interpolated reciprocal depth. */
	float lo = 0, hi = 1;
	for (int axis = 0; axis < 2; ++axis)
	{
		float min = axis ? EditorUI_Top () : 0, max = axis ? EditorUI_Bottom () : (float)glwidth;
		float delta = out[1][axis] - out[0][axis];
		if (fabsf (delta) < 0.000001f) { if (out[0][axis] < min || out[0][axis] > max) return false; continue; }
		float a = (min - out[0][axis]) / delta, b = (max - out[0][axis]) / delta;
		if (a > b) { float tmp = a; a = b; b = tmp; }
		lo = q_max (lo, a); hi = q_min (hi, b); if (lo > hi) return false;
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		float start_value = out[0][axis], delta = out[1][axis] - start_value;
		out[0][axis] = start_value + lo * delta; out[1][axis] = start_value + hi * delta;
	}
	return true;
}
static qboolean Editor3D_ProjectPoint (const vec3_t point, float screen[3])
{
	vec3_t forward, right, up, delta; Editor3D_Basis (forward, right, up);
	VectorSubtract (point, ui.camera, delta);
	float z = DotProduct (delta, forward);
	if (z < EDITOR3D_NEAR) return false;
	screen[0] = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / z;
	screen[1] = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / z;
	screen[2] = EDITOR3D_NEAR / z; return true;
}
static void Editor3D_DrawKnife (cb_context_t *cbx)
{
	float a[3], b[3];
	qboolean start_visible = ui.knife_pending && Editor3D_ProjectPoint (ui.knife_start, a);
	qboolean end_visible = ui.knife_preview_valid && Editor3D_ProjectPoint (ui.knife_preview, b);
	if (start_visible && end_visible) Editor3D_Line (cbx, a, b, 111);
	for (int i = 0; i < 2; ++i)
	{
		if ((i == 0 && !start_visible) || (i == 1 && !end_visible)) continue;
		float *p = i == 0 ? a : b;
		float x0[3] = {p[0] - 5, p[1], p[2]}, x1[3] = {p[0] + 5, p[1], p[2]};
		float y0[3] = {p[0], p[1] - 5, p[2]}, y1[3] = {p[0], p[1] + 5, p[2]};
		Editor3D_Line (cbx, x0, x1, 111); Editor3D_Line (cbx, y0, y1, 111);
	}
}
static void Editor3D_DrawVertices (cb_context_t *cbx, int sector)
{
	if (sector < 0 || !ui.api.sector_vertex_count || !ui.api.sector_vertex) return;
	vec3_t forward, right, up; Editor3D_Basis (forward, right, up);
	int count = ui.api.sector_vertex_count (ui.api.context, sector);
	for (int i = 0; i < count; ++i)
	{
		vec3_t vertex, delta;
		if (!ui.api.sector_vertex (ui.api.context, sector, i, vertex)) continue;
		if (EditorUI_VertexMovesWithDrag (sector, i)) VectorCopy (ui.vertex_preview, vertex);
		VectorSubtract (vertex, ui.camera, delta);
		float z = DotProduct (delta, forward);
		if (z < EDITOR3D_NEAR) continue;
		float x = glwidth * 0.5f + DotProduct (delta, right) * Editor3D_Focal () / z;
		float y = EditorUI_CenterY () - DotProduct (delta, up) * Editor3D_Focal () / z;
		if (x < 0 || x >= glwidth || y < EditorUI_Top () || y >= EditorUI_Bottom ()) continue;
		float depth = EDITOR3D_NEAR / z;
		float a[3] = {x - 4, y, depth}, b[3] = {x + 4, y, depth};
		float c[3] = {x, y - 4, depth}, d[3] = {x, y + 4, depth};
		int color = i == ui.sector_vertex ? 111 : 15;
		Editor3D_Line (cbx, a, b, color); Editor3D_Line (cbx, c, d, color);
	}
}
static void Editor3D_DrawPoints (cb_context_t *cbx)
{
	if (!ui.api.point_count || !ui.api.point_origin) return;
	int count = ui.api.point_count (ui.api.context), selected = EditorUI_PointSelected ();
	for (int i = 0; i < count; ++i)
	{
		vec3_t origin; float screen[3];
		if (!ui.api.point_origin (ui.api.context, i, origin)) continue;
		if (ui.drag == EDIT_DRAG_POINT && ui.drag_index == i) VectorCopy (ui.point_preview, origin);
		editor_map_model_t model;qboolean is_model=ui.api.point_model && ui.api.point_model(ui.api.context,i,&model);
		int color = i == selected ? 15 : 111;
		vec3_t corners[8];
		if(is_model && EditorUI_ModelBounds(i,corners))
			for(unsigned corner=0;corner<8;++corner)for(unsigned axis=0;axis<3;++axis)if(!(corner&(1u<<axis)))
			{
				float edge[2][3];
				if(Editor3D_ProjectEdge(corners[corner],corners[corner|(1u<<axis)],edge))Editor3D_Line(cbx,edge[0],edge[1],color);
			}
		if (!Editor3D_ProjectPoint (origin, screen) || screen[0] < 6 || screen[0] >= glwidth - 6 ||
			screen[1] < EditorUI_Top () + 6 || screen[1] >= EditorUI_Bottom () - 6) continue;
		float a[3] = {screen[0] - 5, screen[1], screen[2]}, b[3] = {screen[0] + 5, screen[1], screen[2]};
		float c[3] = {screen[0], screen[1] - 5, screen[2]}, d[3] = {screen[0], screen[1] + 5, screen[2]};
		Editor3D_Line (cbx, a, b, color); Editor3D_Line (cbx, c, d, color);
		if (is_model)
		{
			float e[3] = {screen[0]-5,screen[1]-5,screen[2]}, f[3] = {screen[0]+5,screen[1]+5,screen[2]};
			float g[3] = {screen[0]-5,screen[1]+5,screen[2]}, h[3] = {screen[0]+5,screen[1]-5,screen[2]};
			Editor3D_Line (cbx,e,f,color);Editor3D_Line (cbx,g,h,color);
			Draw_String(cbx,screen[0]+8,screen[1]-8,model.model);
		}
	}
}
static qboolean Editor3D_Append (editor3d_polygon_t **polys, size_t *count, size_t *capacity, const vec3_t *verts, int n, qboolean selected, qboolean portal, qboolean source_outline)
{
	if (*count >= 131072) return false;
	if (*count == *capacity)
	{
		size_t next = *capacity ? *capacity * 2 : 128;
		editor3d_polygon_t *p = realloc (*polys, next * sizeof (*p));
		if (!p) return false;
		*polys = p; *capacity = next;
	}
	if (Editor3D_ProjectFace (verts, n, selected, &(*polys)[*count]))
	{
		(*polys)[*count].portal = portal;
		(*polys)[*count].source_outline = source_outline;
		++*count;
	}
	return true;
}
static void Editor3D_DrawSourceOutlines (cb_context_t *cbx, int brushes, int selected)
{
	if (!ui.api.brush_face_count || !ui.api.sector_face_id || !ui.api.sector_source_face_vertices) return;
	for (int sector = 0; sector < brushes; ++sector)
	{
		int faces = ui.api.brush_face_count (ui.api.context, sector);
		if (faces <= 0) continue;
		qboolean *seen = calloc ((size_t)faces, sizeof (*seen));
		if (!seen) continue;
		for (int face = 0; face < faces; ++face)
		{
			int source = ui.api.sector_face_id (ui.api.context, sector, face);
			if (source < 0 || source >= faces || seen[source]) continue;
			seen[source] = true;
			qboolean portal = ui.api.sector_face_is_portal && ui.api.sector_face_is_portal (ui.api.context, sector, source);
			qboolean highlight = sector == selected && (ui.sector_mode == EDIT_SECTOR_VOLUME ||
				((ui.sector_mode == EDIT_SECTOR_FACE || ui.sector_mode == EDIT_SECTOR_KNIFE) && source == ui.sector_face));
			if (!highlight && !portal) continue;
			int n = ui.api.sector_source_face_vertices (ui.api.context, sector, source, NULL, 0);
			if (n < 3 || n > 65536) continue;
			vec3_t *verts = malloc ((size_t)n * sizeof (*verts));
			if (!verts) continue;
			if (ui.api.sector_source_face_vertices (ui.api.context, sector, source, verts, n) != n) { free (verts); continue; }
			if (ui.drag == EDIT_DRAG_MOVE && sector == ui.drag_index)
				for (int v = 0; v < n; ++v) for (int axis = 0; axis < 3; ++axis)
					verts[v][axis] += ui.preview_mins[axis] - ui.original_mins[axis];
			if (ui.drag == EDIT_DRAG_FACE && sector == ui.drag_index && source == ui.sector_face)
				for (int v = 0; v < n; ++v) for (int axis = 0; axis < 3; ++axis)
					verts[v][axis] += ui.face_normal[axis] * ui.face_distance;
			qboolean vertex_deforming = false;
			if (ui.drag == EDIT_DRAG_VERTEX)
				for (int v = 0; v < n; ++v)
					if (EditorUI_FaceVertexMovesWithDrag (sector, source, v, false, verts[v]))
					{ VectorCopy (ui.vertex_preview, verts[v]); vertex_deforming = true; }
			for (int v = 0; v < n; ++v)
			{
				editor_edge_range_t ranges[129] = {{0,1}};
				qboolean explicit_face = highlight && (ui.sector_mode == EDIT_SECTOR_FACE || ui.sector_mode == EDIT_SECTOR_KNIFE);
				qboolean deforming = vertex_deforming || (sector == ui.drag_index && ui.drag == EDIT_DRAG_FACE);
				int nranges = portal && !explicit_face && !deforming ? EditorUI_PortalEdgeRanges (sector, source, v, ranges) : 1;
				for (int r = 0; r < nranges; ++r)
				{
					vec3_t a, b; float line[2][3];
					for (int k = 0; k < 3; ++k)
					{
						float delta = verts[(v + 1) % n][k] - verts[v][k];
						a[k] = verts[v][k] + delta * ranges[r].first;
						b[k] = verts[v][k] + delta * ranges[r].last;
					}
					if (Editor3D_ProjectEdge (a, b, line)) Editor3D_Line (cbx, line[0], line[1], highlight ? 15 : 111);
				}
			}
			free (verts);
		}
		free (seen);
	}
}
static void Editor3D_Draw (cb_context_t *cbx)
{
	GL_SetCanvas (cbx, CANVAS_DEFAULT);
	R_BindGraphicsPipeline (cbx, PIPELINE_BASIC_BLEND);
	Draw_Fill (cbx, 0, 0, glwidth, glheight, 0, 1);
	EditorUI_FillClipped (cbx, 0, EditorUI_Top (), glwidth, EditorUI_Bottom () - EditorUI_Top (), 8, 1);
	Draw_EditorClearDepth (cbx);
	editor3d_polygon_t *polys = NULL; size_t count = 0, capacity = 0;
	int selected = EditorUI_Selected ();
	int brushes = ui.api.brush_count ? ui.api.brush_count (ui.api.context) : 0;
	qboolean sectors = EditorUI_SectorMode ();
	if (ui.api.brush_face_count && ui.api.brush_face_vertices)
	{
		for (int i = 0; i < brushes; ++i)
		{
			int faces = ui.api.brush_face_count (ui.api.context, i);
			for (int face = 0; face < faces; ++face)
			{
				int source = sectors && ui.api.sector_face_id ? ui.api.sector_face_id (ui.api.context, i, face) : face;
				if (source < 0) continue;
				qboolean portal = sectors && ui.api.sector_face_is_portal && ui.api.sector_face_is_portal (ui.api.context, i, source);
				qboolean highlight = i == selected && (!sectors || ui.sector_mode == EDIT_SECTOR_VOLUME ||
					((ui.sector_mode == EDIT_SECTOR_FACE || ui.sector_mode == EDIT_SECTOR_KNIFE) && source == ui.sector_face));
				vec3_t verts[EDITOR3D_MAX_FACE];
				int n = ui.api.brush_face_vertices (ui.api.context, i, face, verts, EDITOR3D_MAX_FACE);
				if (n < 3 || n > EDITOR3D_MAX_FACE) continue;
				if (ui.drag == EDIT_DRAG_MOVE && i == ui.drag_index)
					for (int v = 0; v < n; ++v) for (int axis = 0; axis < 3; ++axis)
						verts[v][axis] += ui.preview_mins[axis] - ui.original_mins[axis];
				if (ui.drag == EDIT_DRAG_FACE && i == ui.drag_index && source == ui.sector_face)
					for (int v = 0; v < n; ++v) for (int axis = 0; axis < 3; ++axis)
						verts[v][axis] += ui.face_normal[axis] * ui.face_distance;
				if (ui.drag == EDIT_DRAG_VERTEX)
					for (int v = 0; v < n; ++v)
						if (EditorUI_FaceVertexMovesWithDrag (i, face, v, true, verts[v])) VectorCopy (ui.vertex_preview, verts[v]);
				if (!Editor3D_Append (&polys, &count, &capacity, verts, n, highlight, portal,
					sectors && ui.api.sector_face_id && ui.api.sector_source_face_vertices)) break;
			}
		}
	}
	if (ui.drag == EDIT_DRAG_CREATE && ui.preview_maxs[0] > ui.preview_mins[0] && ui.preview_maxs[1] > ui.preview_mins[1])
	{
		vec3_t verts[8];
		for (int i = 0; i < 8; ++i)
			for (int axis = 0; axis < 3; ++axis) verts[i][axis] = (i & (1 << axis)) ? ui.preview_maxs[axis] : ui.preview_mins[axis];
		static const int indices[6][4] = {{0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}};
		for (int face = 0; face < 6; ++face)
		{
			vec3_t quad[4]; for (int k = 0; k < 4; ++k) VectorCopy (verts[indices[face][k]], quad[k]);
			Editor3D_Append (&polys, &count, &capacity, quad, 4, true, false, false);
		}
	}
	qboolean selected_face_overlay = sectors && (ui.sector_mode == EDIT_SECTOR_FACE || ui.sector_mode == EDIT_SECTOR_KNIFE);
	for (size_t i = 0; i < count; ++i)
		if (!polys[i].portal)
			Draw_EditorPolygonDepth (cbx, (const float (*)[3])polys[i].xyz, polys[i].count,
				polys[i].selected && !selected_face_overlay ? 111 : polys[i].color, 1, true);
	/* Coplanar sector faces can be submitted in either order. Overlay the
	 * selected face after opaque geometry, with depth testing and no depth
	 * writes, so it stays visible without drawing through nearer surfaces. */
	EditorUI_DrawModels (cbx);
	if (selected_face_overlay)
		for (size_t i = 0; i < count; ++i)
			if (polys[i].selected && !polys[i].portal)
			{
				float highlighted[EDITOR3D_MAX_CLIPPED][3];
				for (int v = 0; v < polys[i].count; ++v)
				{
					highlighted[v][0] = polys[i].xyz[v][0]; highlighted[v][1] = polys[i].xyz[v][1];
					highlighted[v][2] = q_min (1.0f, polys[i].xyz[v][2] * 1.00001f);
				}
				Draw_EditorPolygonDepth (cbx, (const float (*)[3])highlighted, polys[i].count, 111, 1, false);
			}
	/* Draw outlines after every opaque face so later geometry can occlude them. */
	for (size_t i = 0; i < count; ++i)
	{
		if (!polys[i].source_outline && (polys[i].selected || polys[i].portal))
			for (int j = 0; j < polys[i].count; ++j)
				Editor3D_Line (cbx, polys[i].xyz[j], polys[i].xyz[(j + 1) % polys[i].count], polys[i].selected ? 15 : 111);
	}
	if (sectors) Editor3D_DrawSourceOutlines (cbx, brushes, selected);
	free (polys);
	if (sectors && ui.sector_mode == EDIT_SECTOR_VERTEX) Editor3D_DrawVertices (cbx, selected);
	if (sectors && ui.sector_mode == EDIT_SECTOR_KNIFE) Editor3D_DrawKnife (cbx);
	Editor3D_DrawPoints (cbx);
	Draw_Fill (cbx, 0, 0, glwidth, EditorUI_Top (), 0, 0.9f);
	Draw_String (cbx, 8, 5, sectors ? "SECTOR EDITOR  |  PERSPECTIVE  |  TAB: TOP  1/2/3/4: views" : "MAP EDITOR  |  PERSPECTIVE  |  TAB: TOP  1/2/3/4: views");
	Draw_String (cbx, 8, 19, "WASD: move  Q/E: down/up  Right-drag: look  Wheel: forward/back");
	Draw_String (cbx, 8, 31, sectors && ui.sector_mode == EDIT_SECTOR_KNIFE ? "KNIFE: click two face edges  Esc: cancel  K: exit  I: inspector" :
		sectors ? "F: face  V: vertex  K: knife  G: create  O: type  H: light  Ctrl+D: duplicate  L: link  F9: play  I: inspector" :
		"Left: select/move  Shift+drag: create  H: light  Ctrl+D: duplicate  [ ]: elevation  F9: play  I: inspector");
	float bottom = EditorUI_Bottom ();
	Draw_Fill (cbx, 0, bottom, glwidth, glheight - bottom, 0, 0.9f);
	char line[256];
	if (sectors)
	{
		char link[48] = "", snap[48] = "";
		if (ui.portal_sector >= 0) q_snprintf (link, sizeof (link), "  LINK %d:%d", ui.portal_sector, ui.portal_face);
		if (ui.drag == EDIT_DRAG_VERTEX && ui.vertex_snap) q_snprintf (snap, sizeof (snap), "  SNAP %d:%d", ui.vertex_snap_sector, ui.vertex_snap_vertex);
		int type = selected >= 0 && ui.api.sector_prop_type ? ui.api.sector_prop_type (ui.api.context, selected) : -1;
		q_snprintf (line, sizeof (line), "CAM %.0f %.0f %.0f  %s  SECTOR %d (%s)  FACE %d  VERTEX %d  CREATE %s%s%s",
			ui.camera[0], ui.camera[1], ui.camera[2], ui.sector_mode == EDIT_SECTOR_FACE ? "FACE" : ui.sector_mode == EDIT_SECTOR_VERTEX ? "VERTEX" : ui.sector_mode == EDIT_SECTOR_KNIFE ? "KNIFE" : "VOLUME",
			selected, type < 0 ? "-" : EditorUI_PropName (type), ui.sector_face, ui.sector_vertex, EditorUI_PropName (ui.create_prop_type), link, snap);
	}
	else q_snprintf (line, sizeof (line), "CAM %.0f %.0f %.0f   GRID %.0f   BRUSHES %d", ui.camera[0], ui.camera[1], ui.camera[2], EDITOR_GRID, brushes);
	if (EditorUI_PointSelected () >= 0) q_snprintf (line + strlen (line), sizeof (line) - strlen (line), "  LIGHT %d", EditorUI_PointSelected ());
	Draw_String (cbx, 8, bottom + 3, line);
	const char *status = EditorUI_StatusText ();
	if (status && *status) Draw_String (cbx, 8, bottom + 17, status);
}
