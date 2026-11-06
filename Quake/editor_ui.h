/* Interactive perspective and orthographic editor views. Include after quakedef.h. */
#ifndef QUAKE_EDITOR_UI_H
#define QUAKE_EDITOR_UI_H
#include <stdint.h>
#include "editor_map.h"

typedef enum
{
	EDITOR_UI_SAVE,
	EDITOR_UI_PLAY,
	EDITOR_UI_UNDO,
	EDITOR_UI_REDO
} editor_ui_action_t;

typedef enum
{
	EDITOR_VIEW_PERSPECTIVE,
	EDITOR_VIEW_TOP,
	EDITOR_VIEW_FRONT,
	EDITOR_VIEW_RIGHT
} editor_ui_view_t;

typedef struct
{
	void *context;
	int (*brush_count) (void *context);
	qboolean (*brush_bounds) (void *context, int index, vec3_t mins, vec3_t maxs);
	int (*brush_face_count) (void *context, int index);
	int (*brush_face_vertices) (void *context, int brush, int face, vec3_t *vertices, int capacity);
	int (*selected) (void *context);
	void (*select) (void *context, int index); /* -1 clears the selection */
	qboolean (*create_box) (void *context, const vec3_t mins, const vec3_t maxs);
	qboolean (*set_bounds) (void *context, int index, const vec3_t mins, const vec3_t maxs);
	void (*remove_selected) (void *context);
	void (*place_spawn) (void *context, const vec3_t origin);
	qboolean (*spawn_origin) (void *context, vec3_t origin);
	void (*action) (void *context, editor_ui_action_t action);
	const char *(*status) (void *context);
	/* Point indices are document entity indices; point_origin returns false for
	 * entities that are not editable point lights. */
	int (*point_count) (void *context);
	qboolean (*point_origin) (void *context, int index, vec3_t origin);
	int (*point_selected) (void *context);
	void (*point_select) (void *context, int index);
	qboolean (*point_create_light) (void *context, const vec3_t origin);
	qboolean (*point_light_color) (void *context, int index, double rgb[3]);
	qboolean (*point_set_light_color) (void *context, int index, const double rgb[3]);
	qboolean (*point_set_origin) (void *context, int index, const vec3_t origin);
	qboolean (*point_remove) (void *context, int index);
	qboolean (*point_model) (void *context, int index, editor_map_model_t *model);
	qboolean (*point_model_bounds) (void *context, int index, vec3_t corners[8]);
	qboolean (*point_set_model) (void *context, int index, const editor_map_model_t *model);
	qboolean (*point_create_model) (void *context, const char *path, const vec3_t origin);
	qboolean (*duplicate_selected) (void *context);
	/* Optional 3D sector editing interface. Existing brush callbacks remain
	 * the display and volume-edit interface in either document mode. */
	qboolean (*sector_mode) (void *context);
	uint64_t (*sector_revision) (void *context);
	/* Sector kind: 0 world, 1 static prop, 2 dynamic prop, 3 movable prop. */
	int (*sector_prop_type) (void *context, int sector);
	qboolean (*sector_set_prop_type) (void *context, int sector, int type);
	qboolean (*sector_collision_enabled) (void *context, int sector);
	qboolean (*sector_set_collision_enabled) (void *context, int sector, qboolean enabled);
	const char *(*world_script) (void *context);
	qboolean (*set_world_script) (void *context, const char *path);
	qboolean (*sector_create_prop) (void *context, const vec3_t mins, const vec3_t maxs, int type);
	int (*sector_face_id) (void *context, int sector, int display_polygon);
	qboolean (*sector_face_is_portal) (void *context, int sector, int face);
	int (*sector_face_portal_peer) (void *context, int sector, int face);
	qboolean (*sector_move_face) (void *context, int sector, int face, const vec3_t delta);
	int (*sector_vertex_count) (void *context, int sector);
	qboolean (*sector_vertex) (void *context, int sector, int vertex, vec3_t position);
	qboolean (*sector_set_vertex) (void *context, int sector, int vertex, const vec3_t position);
	qboolean (*sector_same_vertex) (void *context, int sector, int vertex, int other_sector, int other_vertex);
	/* Exact document-space coincidence, before conversion to float display coordinates. */
	qboolean (*sector_vertex_coincident) (void *context, int sector, int vertex, int other_sector, int other_vertex);
	qboolean (*sector_vertex_same_axis) (void *context, int sector, int vertex, int other_sector, int other_vertex, int axis);
	qboolean (*sector_face_vertex_coincident) (void *context, int sector, int face, int corner,
		int source_sector, int source_vertex, qboolean display_polygon);
	qboolean (*sector_merge_vertex) (void *context, int sector, int vertex, int target_sector, int target_vertex);
	int (*sector_source_face_vertices) (void *context, int sector, int face, vec3_t *vertices, int capacity);
	qboolean (*sector_knife) (void *context, int sector, int face, const vec3_t start, const vec3_t end);
	qboolean (*sector_link_portal) (void *context, int sector, int face, int other_sector, int other_face);
	qboolean (*sector_unlink_portal) (void *context, int sector, int face);
	qboolean (*sector_apply_material) (void *context, int sector, int face);
} editor_ui_adapter_t;

typedef enum
{
	EDITOR_INSPECT_NONE, EDITOR_INSPECT_BRUSH, EDITOR_INSPECT_WORLD,
	EDITOR_INSPECT_STATIC, EDITOR_INSPECT_DYNAMIC, EDITOR_INSPECT_MOVABLE, EDITOR_INSPECT_LIGHT, EDITOR_INSPECT_MODEL
} editor_inspector_kind_t;
typedef struct
{
	editor_inspector_kind_t kind;
	int index;
	qboolean supported, collision_enabled;
	qboolean script_supported;
	qboolean model_supported;
	qboolean light_color_supported;
	double light_color[3];
	editor_map_model_t model;
	char world_script[256];
	uint64_t revision;
} editor_inspector_state_t;

/* Main-thread input and document operations; draw reads a frame-stable state. */
void EditorUI_SetAdapter (const editor_ui_adapter_t *adapter);
void EditorUI_SetActive (qboolean active);
editor_ui_view_t EditorUI_GetView (void);
void EditorUI_SetView (editor_ui_view_t view);
void EditorUI_Focus (const vec3_t origin);
qboolean EditorUI_IsActive (void);
qboolean EditorUI_KeyEvent (int key, qboolean down);
void EditorUI_Update (void);
void EditorUI_CancelDrag (void);
qboolean EditorUI_CancelKnife (void);
void EditorUI_Draw (cb_context_t *cbx);
void EditorUI_SetStatus (const char *message);
void EditorUI_GetInspectorState (editor_inspector_state_t *state);
qboolean EditorUI_SetInspectorCollision (const editor_inspector_state_t *selection, qboolean enabled);
qboolean EditorUI_SetInspectorScript (const editor_inspector_state_t *selection, const char *path);
qboolean EditorUI_SetInspectorLightColor (const editor_inspector_state_t *selection, const double rgb[3]);
qboolean EditorUI_SetInspectorModel (const editor_inspector_state_t *selection, const editor_map_model_t *model);
qboolean EditorUI_CreateModel (const char *path);
/* World arguments/results are the active orthographic view's horizontal and
 * vertical axes: X/Y in top, X/Z in front, and Y/Z in right. */
void EditorUI_ScreenToWorld (float x, float y, float *world_x, float *world_y);
void EditorUI_WorldToScreen (float x, float y, float *screen_x, float *screen_y);

#endif
