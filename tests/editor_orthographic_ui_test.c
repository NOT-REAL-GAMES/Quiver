/* Orthographic viewport regression: reuse the existing headless editor stubs
 * while exercising front/right interactions independently of the main suite. */
#define main editor_original_ui_test_main
#include "editor_ui_test.c"
#include "editor_controls.h"
#undef main

static void Key (int key)
{
	Click (key, true); Click (key, false);
}
static void View (int key)
{
	Key (key); EditorUI_Draw (NULL);
}
static void ExpectNear (float a, float b)
{
	if (fabsf (a - b) > 0.02f) fprintf (stderr, "expected %.3f, got %.3f\n", b, a);
	assert (fabsf (a - b) <= 0.02f);
}
static void Pixel (float h, float v, int *x, int *y)
{
	float sx, sy; EditorUI_WorldToScreen (h, v, &sx, &sy);
	*x = (int)lroundf (sx); *y = (int)lroundf (sy);
}
static vec3_t ortho_vertex;
static vec3_t ortho_target;
static int ortho_vertex_moves, ortho_vertex_merges;
static qboolean ortho_snap, ortho_hidden_alias;
static qboolean inspector_test_enabled, inspector_collision;
static int inspector_changes;
static char inspector_script[256];
static int inspector_script_changes;
static const char *InspectorScript (void *ctx) { (void)ctx; return inspector_script; }
static qboolean InspectorSetScript (void *ctx, const char *path)
{ (void)ctx; q_strlcpy (inspector_script, path, sizeof (inspector_script)); ++inspector_script_changes; ++sector_revision; return true; }
static qboolean InspectorCollision (void *ctx, int sector)
{ (void)ctx; assert (sector == 0); return inspector_collision; }
static qboolean InspectorSetCollision (void *ctx, int sector, qboolean enabled)
{ (void)ctx; assert (sector == 0); inspector_collision = enabled; ++inspector_changes; ++sector_revision; return true; }
static int OrthoVertexCount (void *ctx, int sector)
{ (void)ctx; return count && (sector == 0 || (sector == 1 && ortho_snap)) ? 1 : 0; }
static qboolean OrthoVertex (void *ctx, int sector, int vertex, vec3_t position)
{ (void)ctx; if (vertex != 0 || !count || (sector != 0 && !(sector == 1 && ortho_snap))) return false; VectorCopy (sector ? ortho_target : ortho_vertex, position); return true; }
static qboolean OrthoMoveVertex (void *ctx, int sector, int vertex, const vec3_t position)
{ (void)ctx; assert (sector == 0 && vertex == 0); VectorCopy (position, ortho_vertex); ++ortho_vertex_moves; return true; }
static qboolean OrthoMergeVertex (void *ctx, int sector, int vertex, int target_sector, int target_vertex)
{ (void)ctx; assert (sector == 0 && vertex == 0 && target_sector == 1 && target_vertex == 0); ++ortho_vertex_merges; return true; }
static qboolean OrthoSameAxis (void *ctx, int sector, int vertex, int target_sector, int target_vertex, int axis)
{
	(void)ctx; assert (sector == 0 && vertex == 0 && target_sector == 1 && target_vertex == 0 && axis >= 0 && axis < 3);
	return !ortho_hidden_alias && ortho_vertex[axis] == ortho_target[axis];
}
static void InitOrtho (void)
{
	editor_ui_adapter_t api = {0};
	api.brush_count = BrushCount; api.brush_bounds = Bounds; api.selected = Selected; api.select = SelectWithPoints;
	api.brush_face_count = FaceCount; api.brush_face_vertices = FaceVertices;
	api.create_box = Create; api.set_bounds = SetBounds; api.remove_selected = Remove; api.place_spawn = Place;
	api.action = Action; api.status = SessionStatus;
	api.sector_mode = SectorMode; api.sector_revision = SectorRevision; api.sector_face_id = SectorFaceId;
	api.sector_source_face_vertices = SourceFaceVertices; api.sector_face_is_portal = FacePortal;
	api.sector_vertex_count = OrthoVertexCount; api.sector_vertex = OrthoVertex; api.sector_set_vertex = OrthoMoveVertex;
	api.sector_merge_vertex = OrthoMergeVertex;
	api.sector_vertex_same_axis = OrthoSameAxis;
	api.point_count = PointCount; api.point_origin = PointOrigin; api.point_selected = PointSelected;
	api.point_select = PointSelect; api.point_create_light = PointCreate; api.point_set_origin = PointMove;
	api.point_remove = PointRemove; api.duplicate_selected = DuplicateSelected;
	api.sector_prop_type = PropType; api.sector_set_prop_type = SetPropType; api.sector_create_prop = CreateProp;
	if (inspector_test_enabled) { api.sector_collision_enabled = InspectorCollision; api.sector_set_collision_enabled = InspectorSetCollision;
		api.world_script = InspectorScript; api.set_world_script = InspectorSetScript; }
	EditorUI_SetActive (false); EditorUI_SetAdapter (&api);
	const vec3_t origin = {11, 23, 37}; EditorUI_Focus (origin); EditorUI_SetActive (true);
	count = 0; selected = test_point_selected = -1; test_point_count = 0; test_point_moves = 0; ortho_snap = ortho_hidden_alias = false;
	sector_document = true; crossing_faces = clipping_face = coincident_faces = false;
}
static void TestViewCycleAndProjection (void)
{
	InitOrtho ();
	/* TAB should visit the three actual planes and return to perspective. */
	Key (K_TAB); assert (EditorUI_GetView () == EDITOR_VIEW_TOP); float h, v, x, y;
	EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 11); ExpectNear (v, 23);
	Key (K_TAB); assert (EditorUI_GetView () == EDITOR_VIEW_FRONT); EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 11); ExpectNear (v, 37);
	Key (K_TAB); assert (EditorUI_GetView () == EDITOR_VIEW_RIGHT); EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 23); ExpectNear (v, 37);
	EditorUI_WorldToScreen (55, 69, &x, &y); EditorUI_ScreenToWorld (x, y, &h, &v); ExpectNear (h, 55); ExpectNear (v, 69);
	Key (K_TAB); assert (EditorUI_GetView () == EDITOR_VIEW_PERSPECTIVE);
	camera_x = camera_y = -9999; EditorUI_Draw (NULL); assert (camera_x != -9999 && camera_y != -9999);
	/* Shortcuts reach the same views independently of current mode. */
	View ('2'); EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 11); ExpectNear (v, 37);
	View ('3'); EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 23); ExpectNear (v, 37);
	View ('1'); EditorUI_ScreenToWorld (320, 248, &h, &v); ExpectNear (h, 11); ExpectNear (v, 23);
	View ('4'); camera_x = -9999; EditorUI_Draw (NULL); assert (camera_x != -9999);
}
static void TestPointDrag (int key, int horizontal, int vertical, int hidden)
{
	InitOrtho (); View (key);
	test_point_count = 1; test_point_selected = -1;
	for (int i = 0; i < 3; ++i) test_points[0][i] = 0;
	test_points[0][horizontal] = 16; test_points[0][vertical] = 48; test_points[0][hidden] = 3.25f;
	int x, y; Pixel (16, 48, &x, &y);
	Mouse (x, y); Click (K_MOUSE1, true); assert (test_point_selected == 0);
	Mouse (x + 32, y - 16); Click (K_MOUSE1, false);
	assert (test_point_moves == 1); ExpectNear (test_points[0][horizontal], 48);
	ExpectNear (test_points[0][vertical], 64); ExpectNear (test_points[0][hidden], 3.25f);
	Key (']'); assert (test_point_moves == 2); ExpectNear (test_points[0][hidden], 19.25f);
	ExpectNear (test_points[0][horizontal], 48); ExpectNear (test_points[0][vertical], 64);
}
static void TestSectorMove (int key, int horizontal, int vertical, int hidden)
{
	InitOrtho (); View (key); count = 1; selected = -1; test_point_count = 0;
	for (int i = 0; i < 3; ++i) { brush_mins[i] = 0; brush_maxs[i] = 64; }
	brush_mins[hidden] = 3.25f; brush_maxs[hidden] = 67.25f;
	brush_mins[horizontal] = 16; brush_maxs[horizontal] = 80;
	brush_mins[vertical] = 32; brush_maxs[vertical] = 96;
	int x, y; Pixel (48, 64, &x, &y);
	Mouse (x, y); Click (K_MOUSE1, true); assert (selected == 0);
	Mouse (x + 32, y - 16); Click (K_MOUSE1, false);
	ExpectNear (brush_mins[horizontal], 48); ExpectNear (brush_maxs[horizontal], 112);
	ExpectNear (brush_mins[vertical], 48); ExpectNear (brush_maxs[vertical], 112);
	ExpectNear (brush_mins[hidden], 3.25f); ExpectNear (brush_maxs[hidden], 67.25f);
	Key (']'); ExpectNear (brush_mins[hidden], 19.25f); ExpectNear (brush_maxs[hidden], 83.25f);
	Click (K_SHIFT, true); Key (']'); Click (K_SHIFT, false);
	ExpectNear (brush_mins[hidden], 19.25f); ExpectNear (brush_maxs[hidden], 99.25f);
}
static void TestSectorResize (int key, int horizontal, int vertical, int hidden)
{
	InitOrtho (); View (key); count = 1; selected = 0; test_point_count = 0;
	for (int i = 0; i < 3; ++i) { brush_mins[i] = 0; brush_maxs[i] = 64; }
	brush_mins[hidden] = 3.25f; brush_maxs[hidden] = 67.25f;
	brush_mins[horizontal] = 16; brush_maxs[horizontal] = 80;
	brush_mins[vertical] = 32; brush_maxs[vertical] = 96;
	int x, y; Pixel (80, 32, &x, &y);
	Mouse (x - 2, y - 2); Click (K_MOUSE1, true); Mouse (x + 16, y + 16); Click (K_MOUSE1, false);
	ExpectNear (brush_mins[horizontal], 16); ExpectNear (brush_maxs[horizontal], 96);
	ExpectNear (brush_mins[vertical], 16); ExpectNear (brush_maxs[vertical], 96);
	ExpectNear (brush_mins[hidden], 3.25f); ExpectNear (brush_maxs[hidden], 67.25f);
}
static void TestCreate (int key, int horizontal, int vertical, int hidden, float focus)
{
	InitOrtho (); View (key); count = 0; selected = -1; test_point_count = 0; created_props = 0;
	Key ('g'); /* prop_static, created directly inside the world */
	int x0, y0, x1, y1; Pixel (32, 48, &x0, &y0); Pixel (64, 80, &x1, &y1);
	Click (K_SHIFT, true); Mouse (x0, y0); Click (K_MOUSE1, true);
	Mouse (x1, y1); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (created_props == 1 && created_prop_type == 1);
	ExpectNear (brush_mins[horizontal], 32); ExpectNear (brush_maxs[horizontal], 64);
	ExpectNear (brush_mins[vertical], 48); ExpectNear (brush_maxs[vertical], 80);
	ExpectNear (brush_maxs[hidden] - brush_mins[hidden], 128);
	assert (focus >= brush_mins[hidden] && focus <= brush_maxs[hidden]);
}
static void TestVertexDrag (int key, int horizontal, int vertical, int hidden)
{
	InitOrtho (); View (key); count = 1; selected = 0; test_point_count = 0; ortho_vertex_moves = 0;
	for (int i = 0; i < 3; ++i) { brush_mins[i] = -128; brush_maxs[i] = 128; ortho_vertex[i] = 0; }
	ortho_vertex[horizontal] = 16; ortho_vertex[vertical] = 48; ortho_vertex[hidden] = 3.25f;
	Key ('v'); int x, y; Pixel (16, 48, &x, &y);
	Mouse (x, y); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (ortho_vertex_moves == 0); /* A click must not snap an off-grid depth. */
	Mouse (x, y); Click (K_MOUSE1, true); Mouse (x + 32, y - 16); Click (K_MOUSE1, false);
	assert (ortho_vertex_moves == 1); ExpectNear (ortho_vertex[horizontal], 48);
	ExpectNear (ortho_vertex[vertical], 64); ExpectNear (ortho_vertex[hidden], 3.25f);
}
static void TestVertexWeld (int key, int horizontal, int vertical, int hidden, qboolean same_depth, qboolean hidden_alias)
{
	InitOrtho (); View (key); count = 2; selected = 0; test_point_count = 0; ortho_vertex_moves = ortho_vertex_merges = 0; ortho_snap = true;
	for (int i = 0; i < 3; ++i) { brush_mins[i] = -128; brush_maxs[i] = 128; ortho_vertex[i] = ortho_target[i] = 0; }
	ortho_vertex[horizontal] = 16; ortho_vertex[vertical] = 48; ortho_vertex[hidden] = 3.25f;
	ortho_target[horizontal] = 48; ortho_target[vertical] = 64; ortho_target[hidden] = same_depth ? 3.25f : 19.25f; ortho_hidden_alias = hidden_alias;
	Key ('v'); int x, y; Pixel (16, 48, &x, &y);
	Mouse (x, y); Click (K_MOUSE1, true); Mouse (x + 32, y - 16); Click (K_MOUSE1, false);
	if (same_depth && !hidden_alias) assert (ortho_vertex_merges == 1 && ortho_vertex_moves == 0);
	else { assert (ortho_vertex_merges == 0 && ortho_vertex_moves == 1); ExpectNear (ortho_vertex[hidden], 3.25f); }
}
static void TestLightCreate (int key, int horizontal, int vertical, int hidden, float focus)
{
	InitOrtho (); View (key); count = 0; selected = -1; test_point_count = 0;
	int x, y; Pixel (32, 64, &x, &y); Mouse (x, y); Key ('h');
	assert (test_point_count == 1 && test_point_selected == 0);
	ExpectNear (test_points[0][horizontal], 32); ExpectNear (test_points[0][vertical], 64);
	/* Hidden coordinate is snapped near the focused plane. */
	assert (fabsf (test_points[0][hidden] - focus) <= 8.01f);
}
static void TestPanZoomOddResolution (void)
{
	glwidth = 641; glheight = 481; InitOrtho (); View ('2');
	Mouse (431, 177); float h, v, x, y, after_h, after_v;
	EditorUI_ScreenToWorld (431, 177, &h, &v); Key (K_MWHEELUP);
	EditorUI_ScreenToWorld (431, 177, &after_h, &after_v); ExpectNear (after_h, h); ExpectNear (after_v, v);
	Click (K_MOUSE2, true); Mouse (462, 194); Click (K_MOUSE2, false);
	EditorUI_WorldToScreen (h, v, &x, &y); ExpectNear (x, 462); ExpectNear (y, 194);
	glwidth = 640; glheight = 480;
}
static void TestFarCoordinates (void)
{
	InitOrtho (); const vec3_t far_origin = {1000000, -1000000, 1000000}; EditorUI_Focus (far_origin); View ('2');
	test_point_count = 1; test_point_selected = -1;
	test_points[0][0] = 1000016; test_points[0][1] = -999999.75f; test_points[0][2] = 1000048;
	int x, y; Pixel (test_points[0][0], test_points[0][2], &x, &y);
	Mouse (x, y); Click (K_MOUSE1, true); Mouse (x + 32, y - 16); Click (K_MOUSE1, false);
	assert (test_point_moves == 1); ExpectNear (test_points[0][0], 1000048); ExpectNear (test_points[0][1], -999999.75f);
	ExpectNear (test_points[0][2], 1000064);
}
static void TestVisibleSourceEdges (void)
{
	InitOrtho (); View ('3'); count = 1; selected = 0; outline_fixture = 1; outline_lines = outline_fills = 0;
	EditorUI_Draw (NULL);
	/* The two render triangles project into a right-view quad, with only its
	 * four real boundaries drawn and no artificial diagonal. */
	assert (outline_lines == 4 && outline_fills == 0);
	outline_fixture = 0;
}
static void TestToolbar (void)
{
	glwidth = 1280; glheight = 720; InitOrtho (); View ('4');
	Mouse (1163, 12); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (EditorUI_GetView () == EDITOR_VIEW_FRONT);
	Mouse (1237, 12); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (EditorUI_GetView () == EDITOR_VIEW_RIGHT);
	Mouse (1089, 12); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (EditorUI_GetView () == EDITOR_VIEW_TOP);
	/* A focused toolbar button must not consume the editor Delete shortcut. */
	count = 1; selected = 0; Key (K_DEL); assert (count == 0 && selected == -1);
	/* A press on the toolbar followed by release over the canvas must not
	 * start a scene drag or activate a button released outside its bounds. */
	count = 0; selected = -1;
	Mouse (1237, 12); Click (K_MOUSE1, true); Mouse (640, 368); Click (K_MOUSE1, false);
	assert (EditorUI_GetView () == EDITOR_VIEW_TOP && count == 0);
	/* Right and middle button captures over the bar must not pan the view or
	 * leave a stuck pan when released over the canvas. */
	for (int button = K_MOUSE2; button <= K_MOUSE3; ++button)
	{
		float h0, v0, h1, v1;
		EditorUI_ScreenToWorld (640, 368, &h0, &v0);
		Mouse (1237, 12); Click (button, true); Mouse (640, 368); Click (button, false);
		EditorUI_ScreenToWorld (640, 368, &h1, &v1); ExpectNear (h1, h0); ExpectNear (v1, v0);
		Mouse (640, 368); Click (K_MOUSE2, true); Mouse (656, 368); Click (K_MOUSE2, false);
		EditorUI_ScreenToWorld (640, 368, &h1, &v1); ExpectNear (h1, h0 - 16); ExpectNear (v1, v0);
	}
	Key ('2'); assert (EditorUI_GetView () == EDITOR_VIEW_FRONT);
	Key (K_TAB); assert (EditorUI_GetView () == EDITOR_VIEW_RIGHT);
	EditorUI_SetActive (false); glwidth = 640; glheight = 480;
}
static void TestInspector (void)
{
	glwidth = 1280; glheight = 720; inspector_test_enabled = true;
	InitOrtho (); View ('1'); count = 1; selected = 0; test_prop_type = 1;
	inspector_collision = true; inspector_changes = 0; EditorUI_Update ();
	Mouse (1170, 105); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (!inspector_collision && inspector_changes == 1 && selected == 0);
	Key (K_SPACE); assert (inspector_collision && inspector_changes == 2 && selected == 0);
	/* Changing the source while a click is held must not apply it to a stale selection. */
	Mouse (1170, 105); Click (K_MOUSE1, true); ++sector_revision; Click (K_MOUSE1, false);
	assert (inspector_collision && inspector_changes == 2 && selected == 0);
	/* Panel surface captures right-button panning and wheel input. */
	float h0, v0, h1, v1;
	EditorUI_ScreenToWorld (640, 368, &h0, &v0);
	Mouse (1100, 130); Click (K_MOUSE2, true); Mouse (1110, 140); Click (K_MOUSE2, false);
	EditorUI_ScreenToWorld (640, 368, &h1, &v1); ExpectNear (h1, h0); ExpectNear (v1, v0);
	EditorUI_ScreenToWorld (656, 368, &h0, &v0);
	Click (K_MWHEELUP, true); Click (K_MWHEELUP, false);
	EditorUI_ScreenToWorld (656, 368, &h1, &v1); ExpectNear (h1, h0); ExpectNear (v1, v0);
	EditorUI_ScreenToWorld (640, 368, &h0, &v0);
	Key ('i'); Mouse (1170, 105); Click (K_MOUSE2, true); Mouse (1186, 105); Click (K_MOUSE2, false);
	EditorUI_ScreenToWorld (640, 368, &h1, &v1); assert (h1 < h0); ExpectNear (v1, v0);
	Key ('i'); View ('4'); Mouse (1170, 105); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (!inspector_collision && inspector_changes == 3 && selected == 0);
	/* World script is edited as one undoable action on Enter, and a revision
	 * change while the field has focus must reject the stale edit. */
	test_prop_type = 0; inspector_script[0] = 0; inspector_script_changes = 0; EditorUI_Update ();
	Mouse (1100, 112); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (EditorControls_TextInput ("scripts/ripple.qs")); Key (K_ENTER);
	assert (!strcmp (inspector_script, "scripts/ripple.qs") && inspector_script_changes == 1);
	EditorUI_Update (); Mouse (1100, 112); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (EditorControls_TextInput ("x")); ++sector_revision; Key (K_ENTER);
	assert (!strcmp (inspector_script, "scripts/ripple.qs") && inspector_script_changes == 1);
	/* Brush maps expose the same world property through the inspector. */
	sector_document = false; EditorUI_Update ();
	editor_inspector_state_t brush; EditorUI_GetInspectorState (&brush);
	assert (brush.kind == EDITOR_INSPECT_BRUSH && brush.script_supported);
	assert (EditorUI_SetInspectorScript (&brush, "scripts/brush.qs"));
	assert (!strcmp (inspector_script, "scripts/brush.qs") && inspector_script_changes == 2);
	EditorUI_SetActive (false); inspector_test_enabled = false; glwidth = 640; glheight = 480;
}
int main (void)
{
	TestViewCycleAndProjection ();
	TestPointDrag ('1', 0, 1, 2); TestPointDrag ('2', 0, 2, 1); TestPointDrag ('3', 1, 2, 0);
	TestSectorMove ('1', 0, 1, 2); TestSectorMove ('2', 0, 2, 1); TestSectorMove ('3', 1, 2, 0);
	TestSectorResize ('1', 0, 1, 2); TestSectorResize ('2', 0, 2, 1); TestSectorResize ('3', 1, 2, 0);
	TestVertexDrag ('1', 0, 1, 2); TestVertexDrag ('2', 0, 2, 1); TestVertexDrag ('3', 1, 2, 0);
	TestVertexWeld ('2', 0, 2, 1, true, false); TestVertexWeld ('2', 0, 2, 1, false, false); TestVertexWeld ('2', 0, 2, 1, true, true);
	TestVertexWeld ('3', 1, 2, 0, true, false); TestVertexWeld ('3', 1, 2, 0, false, false); TestVertexWeld ('3', 1, 2, 0, true, true);
	TestCreate ('1', 0, 1, 2, 37); TestCreate ('2', 0, 2, 1, 23); TestCreate ('3', 1, 2, 0, 11);
	TestLightCreate ('1', 0, 1, 2, 85); TestLightCreate ('2', 0, 2, 1, 23); TestLightCreate ('3', 1, 2, 0, 11);
	TestFarCoordinates ();
	TestVisibleSourceEdges ();
	TestToolbar ();
	TestPanZoomOddResolution ();
	TestInspector ();
	puts ("orthographic viewport interaction tests passed");
	return 0;
}
