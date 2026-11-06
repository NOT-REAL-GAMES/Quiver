/* Headless interaction regression for the editor viewport and adapter. */
#include "quakedef.h"
#include "editor_ui.h"
#include "editor_controls.h"
#include "editor_model_preview.h"
#include <assert.h>
#include <stdarg.h>

int glwidth = 640, glheight = 480;
double realtime;
static int mouse_x, mouse_y, count, selected = -1, actions, spawns;
static int polygons;
static qboolean tetrahedron;
static qboolean crossing_faces;
static qboolean clipping_face;
static qboolean coincident_faces;
static int outline_fixture, outline_lines, outline_fills;
static int portal_outline_case, portal_outline_diagonals;
static float portal_outline_segments[64][4];
static qboolean portal_outline_horizontal, portal_outline_far;
static qboolean sector_document;
static qboolean coincident_preview_scene, coincident_preview_portal;
static float coincident_preview_x[3];
static int coincident_preview_fills, coincident_preview_source_checks, coincident_preview_display_checks;
static qboolean snap_scene, snap_target_enabled, snap_alias_enabled, merge_fail;
static qboolean fail_portal;
static uint64_t sector_revision = 1;
static int face_moves, vertex_moves, portal_links, portal_unlinks, materials;
static int vertex_merges, merge_sector, merge_vertex;
static int knife_cuts, knife_sector, knife_face;
static int test_prop_type, created_prop_type, created_props;
static qboolean reject_prop_type;
static qboolean knife_fail;
static vec3_t knife_start, knife_end;
static vec3_t snap_target = {130, 7, 64};
static vec3_t moved_face_delta, moved_vertex;
static const char *session_status = "Ready";
static char footer_status[256];
static float crossing_polygons[2][3][3];
static float clipped_polygon[80][3];
static int crossing_count, crossing_outlines, clipped_count, depth_clears;
static qboolean check_face_highlight;
static int highlight_opaque_count, highlight_fill_count;
static float highlight_second_x;
static float camera_x, camera_y;
static vec3_t brush_mins, brush_maxs, spawn;
static vec3_t test_points[8];
static int test_point_count, test_point_selected = -1, test_point_moves, test_point_duplicates, test_point_removals;
static editor_map_model_t test_models[8];
static qboolean test_is_model[8];
static int test_model_updates,test_model_bounds_reads,test_model_labels;
static double test_light_colors[8][3];
static int test_light_changes, test_light_swatches;
static qboolean test_light_reject;
void VectorMA (const vec3_t a, float scale, const vec3_t b, vec3_t out)
{ for (int i = 0; i < 3; ++i) out[i] = a[i] + scale * b[i]; }
void VectorScale (const vec3_t in, vec_t scale, vec3_t out)
{ for (int i = 0; i < 3; ++i) out[i] = in[i] * scale; }
void CrossProduct (const vec3_t a, const vec3_t b, vec3_t out)
{ out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0]; }
void IN_GetMousePos (int *x, int *y) { *x = mouse_x; *y = mouse_y; }
void GL_SetCanvas (cb_context_t *cbx, canvastype canvas) { (void)cbx; (void)canvas; }
qboolean EditorModelPreview_Perspective (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t camera, const vec3_t forward, const vec3_t right, const vec3_t up, float focal)
{ (void)width;(void)height;(void)top;(void)bottom;(void)camera;(void)forward;(void)right;(void)up;(void)focal;memset(view,0,sizeof(*view));return true; }
qboolean EditorModelPreview_Ortho (editor_model_view_t *view, int width, int height,
	int top, int bottom, const vec3_t center, int horizontal, int vertical, int depth, float zoom)
{ (void)width;(void)height;(void)top;(void)bottom;(void)center;(void)horizontal;(void)vertical;(void)depth;(void)zoom;memset(view,0,sizeof(*view));return true; }
qboolean EditorModelPreview_Draw (cb_context_t *cbx, const editor_map_model_t *placement, const editor_model_view_t *view)
{ (void)cbx;(void)placement;(void)view;return true; }
void GL_SetCanvasColor (float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void Draw_Character (cb_context_t *cbx, float x, float y, int c) { (void)cbx; (void)x; (void)y; (void)c; }
void Draw_FillRGBA (cb_context_t *cbx, float x, float y, float w, float h, float r, float g, float b, float a)
{
    (void)cbx; (void)x; (void)y; (void)w; (void)a;
    if (h==18 && fabsf(r-1.0f/255)<1e-6f && fabsf(g-64.0f/255)<1e-6f && b==1) ++test_light_swatches;
}
void R_BindGraphicsPipeline (cb_context_t *cbx, graphics_pipeline_t pipeline) { (void)cbx; (void)pipeline; }
void Draw_Fill (cb_context_t *cbx, float x, float y, float w, float h, int c, float a)
{ (void)cbx; (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; }
void Draw_String (cb_context_t *cbx, float x, float y, const char *str)
{
	(void)cbx; (void)x;
	if(!strncmp(str,"progs/",6))++test_model_labels;
	if (!strncmp (str, "CAM ", 4)) sscanf (str + 4, "%f %f", &camera_x, &camera_y);
	if (y == 465) snprintf (footer_status, sizeof (footer_status), "%s", str);
}
void Draw_EditorClearDepth (cb_context_t *cbx) { (void)cbx; ++depth_clears; }
void Draw_EditorPolygonDepth (cb_context_t *cbx, const float (*xyz)[3], int n, int color, float alpha, qboolean depth_write)
{
	(void)cbx; (void)color; (void)alpha; assert (n >= 3); polygons++;
	if (outline_fixture) { if (depth_write) ++outline_fills; else ++outline_lines; }
	if (coincident_preview_scene && depth_write && n == 3 && coincident_preview_fills < 3)
		coincident_preview_x[coincident_preview_fills++] = xyz[0][0];
	if (outline_fixture == 3 && !depth_write && n == 4)
	{
		if (outline_lines > 0 && outline_lines <= 64)
		{
			float *s = portal_outline_segments[outline_lines - 1];
			s[0] = (xyz[0][0] + xyz[3][0]) * 0.5f; s[1] = (xyz[0][1] + xyz[3][1]) * 0.5f;
			s[2] = (xyz[1][0] + xyz[2][0]) * 0.5f; s[3] = (xyz[1][1] + xyz[2][1]) * 0.5f;
		}
		/* Both test rectangles are axis aligned after projection. A diagonal
		 * here reveals a portal seam, regardless of duplicated outer edges. */
		float dx = (xyz[1][0] + xyz[2][0] - xyz[0][0] - xyz[3][0]) * 0.5f;
		float dy = (xyz[1][1] + xyz[2][1] - xyz[0][1] - xyz[3][1]) * 0.5f;
		if (fabsf (dx) > 1.0f && fabsf (dy) > 1.0f) ++portal_outline_diagonals;
	}
	for (int i = 0; i < n; ++i) assert (xyz[i][2] >= 0 && xyz[i][2] <= 1);
	if (check_face_highlight)
	{
		if (depth_write)
		{
			/* Coplanar source faces render neutrally before the chosen face is
			 * overlaid, so the last opaque draw cannot hide the selection. */
			assert (color != 111);
			++highlight_opaque_count;
		}
		else if (color == 111)
		{
			assert (highlight_opaque_count == 4 && n == 4);
			++highlight_fill_count;
			highlight_second_x = xyz[1][0];
		}
	}
	if (crossing_faces && depth_write)
	{
		assert (n == 3 && crossing_count < 2);
		memcpy (crossing_polygons[crossing_count++], xyz, sizeof (crossing_polygons[0]));
	}
	else if (crossing_faces && !depth_write)
	{
		assert (crossing_count == 2); /* Outlines must follow every opaque face. */
		++crossing_outlines;
	}
	if (clipping_face && depth_write)
	{
		assert (n <= 80);
		clipped_count = n;
		memcpy (clipped_polygon, xyz, n * sizeof (xyz[0]));
	}
}
size_t q_strlcpy (char *dst, const char *src, size_t size)
{
	size_t n = strlen (src);
	if (size) { size_t c = n < size - 1 ? n : size - 1; memcpy (dst, src, c); dst[c] = 0; }
	return n;
}
int q_snprintf (char *dst, size_t size, const char *fmt, ...)
{
	va_list ap; va_start (ap, fmt); int n = vsnprintf (dst, size, fmt, ap); va_end (ap); return n;
}
static int BrushCount (void *ctx) { (void)ctx; return count; }
static qboolean Bounds (void *ctx, int index, vec3_t mins, vec3_t maxs)
{
	(void)ctx; if (index != 0 || !count) return false;
	VectorCopy (brush_mins, mins); VectorCopy (brush_maxs, maxs); return true;
}
static int Selected (void *ctx) { (void)ctx; return selected; }
static int FaceCount (void *ctx, int index)
{ (void)ctx; return coincident_preview_scene ? (index >= 0 && index < count ? 1 : 0) : outline_fixture ? ((index == 0 || (outline_fixture == 3 && portal_outline_case == 9 && index == 1)) && count ? (outline_fixture == 3 && portal_outline_case == 8 ? 4 : 2) : 0) : snap_scene ? 0 : crossing_faces || clipping_face || coincident_faces ? (index >= 0 && index < count ? (coincident_faces ? 2 : 1) : 0) : index == 0 && count ? (tetrahedron ? 4 : 6) : 0; }
static int FaceVertices (void *ctx, int index, int face, vec3_t *vertices, int capacity)
{
	(void)ctx;
	if (coincident_preview_scene)
	{
		static const vec3_t triangles[3][3] = {
			{{128,-16,64},{128,16,64},{128,-16,96}},
			{{128,-16,64},{128,-48,64},{128,-16,112}},
			{{128,-16,64},{128,16,48},{128,-48,112}}
		};
		if (index < 0 || index >= 3 || face != 0 || capacity < 3) return 0;
		memcpy (vertices, triangles[index], sizeof (triangles[index])); return 3;
	}
	if (outline_fixture == 3)
	{
		/* Each source polygon is a portal fragment. Cases 1-6 share a
		 * diagonal; case 7 shares only half a long edge (a T-junction), and
		 * case 8 contains two separate openings. */
		static const float quad[4][2] = {{-32,48},{32,48},{32,112},{-32,112}};
		static const float partial[6][2] = {{-32,60},{32,60},{32,124},{0,60},{-32,60},{0,30}};
		static const int tris[2][3] = {{0,1,2},{0,2,3}};
		static const int opposite_tris[2][3] = {{1,0,3},{1,3,2}};
		int faces = portal_outline_case == 8 ? 4 : 2;
		if ((index != 0 && !(portal_outline_case == 9 && index == 1)) || face < 0 || face >= faces || capacity < 3) return 0;
		for (int i = 0; i < 3; ++i)
		{
			int vertex = portal_outline_case == 9 && index == 1 ? opposite_tris[face][i] : tris[face % 2][i];
			float y = portal_outline_case == 7 ? partial[face * 3 + i][0] : quad[vertex][0];
			float z = portal_outline_case == 7 ? partial[face * 3 + i][1] : quad[vertex][1];
			if (portal_outline_case == 8) y = y * 0.45f + (face < 2 ? -35 : 35);
			if (portal_outline_case == 10 && face == 1) y += 0.5f;
			if (portal_outline_horizontal) { vertices[i][0] = 128 + z - 80; vertices[i][1] = y; vertices[i][2] = portal_outline_case == 6 && face == 1 ? 96 : 80; }
			else { vertices[i][0] = portal_outline_case == 6 && face == 1 ? 144 : 128; vertices[i][1] = y; vertices[i][2] = z; }
			/* A shallow but genuine fold around the common diagonal. */
			if (portal_outline_case == 11 && face == 1 && i == 2)
				vertices[i][portal_outline_horizontal ? 2 : 0] += 0.0625f;
			if (portal_outline_far) for (int axis = 0; axis < 3; ++axis) vertices[i][axis] += 1000000.0f;
		}
		return 3;
	}
	if (outline_fixture)
	{
		static const vec3_t quad[4] = {{128,-64,16}, {128,64,16}, {128,64,144}, {128,-64,144}};
		static const int tris[2][3] = {{0,1,2},{0,2,3}};
		if (index != 0 || face < 0 || face >= 2 || capacity < 3) return 0;
		for (int i = 0; i < 3; ++i) VectorCopy (quad[tris[face][i]], vertices[i]);
		return 3;
	}
	if (coincident_faces)
	{
		/* Each source face is submitted twice, as with a triangulated polygon.
		 * Both sectors' faces are exactly coplanar, with opposite winding. */
		static const vec3_t points[2][4] = {
			{{128,-64,16}, {128,64,16}, {128,64,144}, {128,-64,144}},
			{{128,-64,16}, {128,-64,144}, {128,64,144}, {128,64,16}}
		};
		if (index < 0 || index >= 2 || face < 0 || face >= 2 || capacity < 4) return 0;
		memcpy (vertices, points[index], sizeof (points[index]));
		return 4;
	}
	if (clipping_face)
	{
		static const vec3_t triangle[3] = {{100,-200,80}, {200,0,80}, {100,0,130}};
		if (index != 0 || face != 0 || capacity < 3) return 0;
		memcpy (vertices, triangle, sizeof (triangle));
		return 3;
	}
	if (crossing_faces)
	{
		/* Same screen-space triangle, with opposite depth slopes. */
		static const vec3_t faces[2][3] = {
			{{100,-25,55}, {400,100,-20}, {100,0,105}},
			{{400,-100,-20}, {100,25,55}, {400,0,180}}
		};
		if (index < 0 || index >= 2 || face != 0 || capacity < 3) return 0;
		memcpy (vertices, faces[index], sizeof (faces[index]));
		return 3;
	}
	if (index || !count || face < 0 || face >= (tetrahedron ? 4 : 6) || capacity < 4) return 0;
	if (tetrahedron)
	{
		const float v[4][3] = {{128,-16,64}, {128,16,64}, {128,-16,96}, {160,-16,64}};
		static const int triangles[4][3] = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
		for (int i = 0; i < 3; ++i) VectorCopy (v[triangles[face][i]], vertices[i]);
		return 3;
	}
	static const int table[6][4] = {{0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 3; ++j)
			vertices[i][j] = table[face][i] & (1 << j) ? brush_maxs[j] : brush_mins[j];
	return 4;
}
static void Select (void *ctx, int index) { (void)ctx; selected = index; }
static void SelectWithPoints (void *ctx, int index) { (void)ctx; selected = index; test_point_selected = -1; }
static int PointCount (void *ctx) { (void)ctx; return test_point_count; }
static qboolean PointOrigin (void *ctx, int index, vec3_t point)
{ (void)ctx; if (index < 0 || index >= test_point_count) return false; VectorCopy (test_points[index], point); return true; }
static int PointSelected (void *ctx) { (void)ctx; return test_point_selected; }
static void PointSelect (void *ctx, int index) { (void)ctx; test_point_selected = index; if (index >= 0) selected = -1; }
static qboolean PointCreate (void *ctx, const vec3_t point)
{
	(void)ctx; assert (test_point_count < 8); VectorCopy (point, test_points[test_point_count]);
	test_is_model[test_point_count]=false;
	for(unsigned i=0;i<3;++i)test_light_colors[test_point_count][i]=1;
	test_point_selected = test_point_count++; selected = -1; ++sector_revision; return true;
}
static qboolean PointMove (void *ctx, int index, const vec3_t point)
{
	(void)ctx; assert (index >= 0 && index < test_point_count); VectorCopy (point, test_points[index]);
	++test_point_moves; ++sector_revision; return true;
}
static qboolean PointRemove (void *ctx, int index)
{
	(void)ctx; assert (index >= 0 && index < test_point_count); test_point_selected = -1; ++test_point_removals; return true;
}
static qboolean DuplicateSelected (void *ctx)
{
	(void)ctx; ++test_point_duplicates;
	if (test_point_selected >= 0)
	{
		int source=test_point_selected;editor_map_model_t model=test_models[source];qboolean is_model=test_is_model[source];
		vec3_t point; VectorCopy (test_points[source], point); point[1] += 16;
		if(!PointCreate(ctx,point))return false;
		test_is_model[test_point_selected]=is_model;test_models[test_point_selected]=model;return true;
	}
	return selected >= 0;
}
static qboolean LightColor (void *ctx, int index, double rgb[3])
{
	(void)ctx;
	if (index < 0 || index >= test_point_count || test_is_model[index]) return false;
	memcpy (rgb, test_light_colors[index], sizeof(test_light_colors[index])); return true;
}
static qboolean SetLightColor (void *ctx, int index, const double rgb[3])
{
	(void)ctx;
	if (index < 0 || index >= test_point_count || test_is_model[index] || test_light_reject) return false;
	memcpy (test_light_colors[index], rgb, sizeof(test_light_colors[index]));
	++test_light_changes; ++sector_revision; return true;
}
static qboolean ModelGet(void *ctx,int index,editor_map_model_t *model)
{
	(void)ctx;if(index<0 || index>=test_point_count || !test_is_model[index])return false;
	*model=test_models[index];for(unsigned a=0;a<3;++a)model->origin[a]=test_points[index][a];return true;
}
static qboolean ModelSet(void *ctx,int index,const editor_map_model_t *model)
{
	(void)ctx;if(index<0 || index>=test_point_count || !test_is_model[index] || model->modelscale<=0 || model->skin<0)return false;
	test_models[index]=*model;for(unsigned a=0;a<3;++a)test_points[index][a]=(float)model->origin[a];
	++test_model_updates;++sector_revision;return true;
}
static qboolean ModelCreate(void *ctx,const char *path,const vec3_t origin)
{
	if(strlen(path)>=EDITOR_MAP_MODEL_PATH_MAX || !PointCreate(ctx,origin))return false;
	int index=test_point_selected;test_is_model[index]=true;memset(&test_models[index],0,sizeof(test_models[index]));
	q_strlcpy(test_models[index].model,path,sizeof(test_models[index].model));
	test_models[index].modelscale=1;test_models[index].collision=true;return true;
}
static qboolean ModelBounds(void *ctx,int index,vec3_t corners[8])
{
	editor_map_model_t model;if(!ModelGet(ctx,index,&model))return false;++test_model_bounds_reads;
	for(unsigned v=0;v<8;++v)for(unsigned a=0;a<3;++a)corners[v][a]=(float)(model.origin[a]+((v&(1u<<a))?16:-16)*model.modelscale);
	return true;
}
static qboolean Create (void *ctx, const vec3_t mins, const vec3_t maxs)
{
	(void)ctx; count++; selected = 0; VectorCopy (mins, brush_mins); VectorCopy (maxs, brush_maxs); return true;
}
static int PropType (void *ctx, int sector) { (void)ctx; return sector == 0 ? test_prop_type : -1; }
static qboolean SetPropType (void *ctx, int sector, int type)
{
	(void)ctx; assert (sector == 0 && type >= 0 && type <= 3);
	if (reject_prop_type) { session_status = "Detailed prop error"; return false; }
	test_prop_type = type; return true;
}
static qboolean CreateProp (void *ctx, const vec3_t mins, const vec3_t maxs, int type)
{
	(void)ctx; assert (type >= 1 && type <= 3);
	created_prop_type = type; ++created_props;
	return Create (ctx, mins, maxs);
}
static qboolean SetBounds (void *ctx, int index, const vec3_t mins, const vec3_t maxs)
{
	(void)ctx; assert (index == 0); VectorCopy (mins, brush_mins); VectorCopy (maxs, brush_maxs); return true;
}
static void Remove (void *ctx) { (void)ctx; count = 0; selected = -1; }
static void Place (void *ctx, const vec3_t origin) { (void)ctx; VectorCopy (origin, spawn); spawns++; }
static void Action (void *ctx, editor_ui_action_t action) { (void)ctx; actions |= 1 << action; }
static const char *SessionStatus (void *ctx) { (void)ctx; return session_status; }
static qboolean SectorMode (void *ctx) { (void)ctx; return sector_document; }
static uint64_t SectorRevision (void *ctx) { (void)ctx; return sector_revision; }
static int SectorFaceId (void *ctx, int sector, int polygon) { (void)ctx; (void)sector; return coincident_faces || outline_fixture == 1 ? 0 : polygon; }
static qboolean FacePortal (void *ctx, int sector, int face) { (void)ctx; return coincident_preview_scene ? coincident_preview_portal && sector == 0 : outline_fixture == 3 && !(portal_outline_case == 5 && face == 1); }
static int SectorFacePortalPeer (void *ctx, int sector, int face)
{ (void)ctx; return portal_outline_case == 9 ? 1 - sector : portal_outline_case == 4 && face == 1 ? 2 : 1; }
static qboolean MoveFace (void *ctx, int sector, int face, const vec3_t delta)
{ (void)ctx; assert (sector == 0 && face == 0); VectorCopy (delta, moved_face_delta); ++face_moves; return true; }
static int SectorVertexCount (void *ctx, int sector) { (void)ctx; return coincident_preview_scene ? (sector >= 0 && sector < 3 ? 1 : 0) : outline_fixture == 3 ? 0 : snap_scene ? (sector >= 0 && sector <= (snap_alias_enabled ? 2 : 1) && (sector == 0 || snap_target_enabled) ? 1 : 0) : (sector >= 0 && sector < 2 ? 3 : 0); }
static qboolean SectorVertex (void *ctx, int sector, int vertex, vec3_t position)
{
	if (snap_scene)
	{
		if (vertex || sector < 0 || sector > (snap_alias_enabled ? 2 : 1) || (sector > 0 && !snap_target_enabled)) return false;
		if (sector == 0) { position[0] = 130; position[1] = -25; position[2] = 64; }
		else if (sector == 1 && snap_alias_enabled) { position[0] = 130; position[1] = -25; position[2] = 64; }
		else VectorCopy (snap_target, position);
		return true;
	}
	if (coincident_preview_scene)
	{
		if (sector < 0 || sector >= 3 || vertex != 0) return false;
		position[0] = 128; position[1] = -16; position[2] = 64; return true;
	}
	vec3_t points[4];
	if (vertex < 0 || vertex >= 3 || FaceVertices (ctx, sector, 0, points, 4) != 3) return false;
	VectorCopy (points[vertex], position); return true;
}
static qboolean SetVertex (void *ctx, int sector, int vertex, const vec3_t position)
{ (void)ctx; assert (sector == 0 && vertex == 0); VectorCopy (position, moved_vertex); ++vertex_moves; return true; }
static qboolean SameVertex (void *ctx, int sector, int vertex, int other_sector, int other_vertex)
{ (void)ctx; assert (sector == 0 && vertex == 0 && other_vertex == 0); return other_sector == 0 || (snap_alias_enabled && other_sector == 1); }
static qboolean CoincidentVertex (void *ctx, int sector, int vertex, int other_sector, int other_vertex)
{
	(void)ctx; assert (coincident_preview_scene && vertex == 0 && other_vertex == 0);
	return sector < 2 && other_sector < 2;
}
static qboolean CoincidentFaceVertex (void *ctx, int sector, int face, int corner,
	int source_sector, int source_vertex, qboolean display_polygon)
{
	(void)ctx; assert (coincident_preview_scene && face == 0 && source_sector == 0 && source_vertex == 0);
	if (display_polygon) ++coincident_preview_display_checks; else ++coincident_preview_source_checks;
	/* Sector 2 deliberately projects to the same float coordinates while its
	 * source vertex differs in double precision. */
	return sector < 2 && corner == 0;
}
static qboolean MergeVertex (void *ctx, int sector, int vertex, int target_sector, int target_vertex)
{
	(void)ctx; assert (sector == 0 && vertex == 0);
	merge_sector = target_sector; merge_vertex = target_vertex; ++vertex_merges;
	if (merge_fail) { session_status = "Detailed weld error"; return false; }
	return true;
}
static qboolean LinkPortal (void *ctx, int sector, int face, int other_sector, int other_face)
{
	(void)ctx; assert (sector == 0 && face == 0 && other_sector == 1 && other_face == 0);
	if (fail_portal) { session_status = "Detailed portal validation error"; return false; }
	++portal_links; return true;
}
static qboolean UnlinkPortal (void *ctx, int sector, int face)
{ (void)ctx; assert (sector == 1 && face == 0); ++portal_unlinks; return true; }
static qboolean ApplyMaterial (void *ctx, int sector, int face)
{ (void)ctx; assert (sector == 1 && face == 0); ++materials; return true; }
static int SourceFaceVertices (void *ctx, int sector, int face, vec3_t *vertices, int capacity)
{
	if (outline_fixture == 1)
	{
		static const vec3_t quad[4] = {{128,-64,16}, {128,64,16}, {128,64,144}, {128,-64,144}};
		if (sector != 0 || face != 0) return 0;
		if (vertices && capacity >= 4) memcpy (vertices, quad, sizeof (quad));
		return 4;
	}
	vec3_t points[64];
	int n = FaceVertices (ctx, sector, face, points, 64);
	if (vertices && capacity >= n) memcpy (vertices, points, (size_t)n * sizeof (*vertices));
	return n;
}
static qboolean KnifeCut (void *ctx, int sector, int face, const vec3_t start, const vec3_t end)
{
	(void)ctx; knife_sector = sector; knife_face = face;
	VectorCopy (start, knife_start); VectorCopy (end, knife_end); ++knife_cuts;
	if (knife_fail) { session_status = "Knife validation error"; return false; }
	return true;
}
static void Mouse (int x, int y) { mouse_x = x; mouse_y = y; EditorUI_Update (); }
static void Click (int key, qboolean down) { assert (EditorUI_KeyEvent (key, down)); }
static void FaceClick (int x, int y)
{
	Mouse (x, y); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
}
static void ResetFaceCycling (void)
{
	const vec3_t origin = {0, 0, 32};
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	selected = -1; Click ('f', true); Click ('f', false);
}
static void TestVertexSnapping (void)
{
	const vec3_t origin = {0, 0, 32};
	const int initial_moves = vertex_moves, initial_merges = vertex_merges;
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	sector_document = snap_scene = true; snap_target_enabled = false; count = 2; selected = 0;
	Click ('v', true); Click ('v', false);
	/* An off-grid vertex must not jump when merely selected. */
	Mouse (370, 280); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (vertex_moves == initial_moves && vertex_merges == initial_merges);
	/* Horizontal motion aligns its world Y to the grid without changing the
	 * off-grid X coordinate (the viewing-depth axis). */
	Mouse (370, 280); Click (K_MOUSE1, true); Mouse (338, 280); Click (K_MOUSE1, false);
	assert (vertex_moves == initial_moves + 1 && moved_vertex[0] == 130 && moved_vertex[1] == -16 && moved_vertex[2] == 64);
	/* Nearby off-grid vertices in another sector are welded explicitly. */
	snap_target_enabled = true;
	Mouse (370, 280); Click (K_MOUSE1, true); Mouse (306, 280); Click (K_MOUSE1, false);
	assert (vertex_merges == initial_merges + 1 && merge_sector == 1 && merge_vertex == 0 && vertex_moves == initial_moves + 1);
	/* A rejected weld must not degrade into a position-only move. */
	merge_fail = true;
	Mouse (370, 280); Click (K_MOUSE1, true); Mouse (306, 280); Click (K_MOUSE1, false);
	assert (vertex_merges == initial_merges + 2 && vertex_moves == initial_moves + 1);
	EditorUI_Draw (NULL); assert (!strcmp (footer_status, "Detailed weld error"));
	merge_fail = false; session_status = "Ready";
	/* A vertex along the same screen ray but far away in depth is ignored. */
	snap_target[0] = 300; snap_target[1] = 16; snap_target[2] = 43;
	Mouse (370, 280); Click (K_MOUSE1, true); Mouse (338, 280); Click (K_MOUSE1, false);
	assert (vertex_merges == initial_merges + 2 && vertex_moves == initial_moves + 2 && moved_vertex[1] == -16);
	/* Already-coincident vertices can be welded deliberately by dragging
	 * away and back. Shared aliases must not mask a distinct vertex. */
	snap_alias_enabled = true; count = 3;
	snap_target[0] = 130; snap_target[1] = -25; snap_target[2] = 64;
	Mouse (370, 280); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (vertex_merges == initial_merges + 2);
	Mouse (370, 280); Click (K_MOUSE1, true); Mouse (338, 280); Mouse (370, 280); Click (K_MOUSE1, false);
	assert (vertex_merges == initial_merges + 3 && merge_sector == 2 && merge_vertex == 0 && vertex_moves == initial_moves + 2);
	snap_target[0] = 130; snap_target[1] = 7; snap_target[2] = 64;
	EditorUI_SetActive (false); EditorUI_SetActive (true);
	snap_scene = snap_target_enabled = snap_alias_enabled = sector_document = false; selected = -1;
}
static void TestFaceCycling (void)
{
	const double saved_time = realtime;
	const int saved_links = portal_links, saved_moves = face_moves;
	sector_document = true; coincident_faces = true; crossing_faces = false; count = 2;

	/* L may be pressed between two clicks, with no time deadline. Duplicate
	 * display polygons map to one source face and must not consume a cycle. */
	ResetFaceCycling ();
	FaceClick (320, 248); assert (selected == 0);
	check_face_highlight = true; highlight_opaque_count = highlight_fill_count = 0;
	EditorUI_Draw (NULL);
	assert (highlight_opaque_count == 4 && highlight_fill_count == 2);
	const float first_highlight_x = highlight_second_x;
	Click ('l', true); Click ('l', false);
	realtime += 100;
	FaceClick (324, 248); assert (selected == 1);
	highlight_opaque_count = highlight_fill_count = 0;
	EditorUI_Draw (NULL);
	assert (highlight_opaque_count == 4 && highlight_fill_count == 2 && highlight_second_x != first_highlight_x);
	check_face_highlight = false;
	Click ('l', true); Click ('l', false); assert (portal_links == saved_links + 1);

	/* Repeated nearby clicks wrap; leaving the radius starts at the nearest. */
	ResetFaceCycling ();
	FaceClick (320, 248); assert (selected == 0);
	FaceClick (324, 248); assert (selected == 1);
	FaceClick (322, 248); assert (selected == 0);
	FaceClick (350, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);

	/* Moving the camera or changing the document invalidates the old hit set. */
	Click (K_MWHEELUP, true); Click (K_MWHEELUP, false);
	FaceClick (354, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);
	Mouse (354, 248); Click (K_MOUSE2, true); Mouse (359, 248); Click (K_MOUSE2, false);
	FaceClick (354, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);
	glwidth = 660;
	FaceClick (354, 248); assert (selected == 0);
	glwidth = 640;
	FaceClick (354, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);
	++sector_revision; EditorUI_Update ();
	FaceClick (354, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);
	Click ('f', true); Click ('f', false); Click ('f', true); Click ('f', false);
	FaceClick (354, 248); assert (selected == 0);
	FaceClick (354, 248); assert (selected == 1);
	FaceClick (10, 248); assert (selected == -1);
	FaceClick (320, 248); assert (selected == 0);

	/* Dragging after a click moves the face already selected, rather than a
	 * speculatively cycled face. It also resets the next click to nearest. */
	ResetFaceCycling ();
	FaceClick (320, 248); assert (selected == 0);
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (320, 200); Click (K_MOUSE1, false);
	assert (face_moves == saved_moves + 1 && selected == 0);
	FaceClick (320, 248); assert (selected == 0);

	/* A little normal hand motion is still a click and advances the cycle. */
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (322, 249); Click (K_MOUSE1, false);
	assert (selected == 1 && face_moves == saved_moves + 1);
	/* Moving beyond the drag threshold and returning to the start must not
	 * count as a click, even though the final delta is zero. */
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (320, 238); Mouse (320, 248); Click (K_MOUSE1, false);
	FaceClick (320, 248); assert (selected == 0);

	EditorUI_SetActive (false); EditorUI_SetActive (true);
	coincident_faces = false; sector_document = false; selected = -1;
	realtime = saved_time;
}
static float DepthAt (const float p[3][3], float x, float y)
{
	float det = (p[1][1] - p[2][1]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][1] - p[2][1]);
	assert (fabsf (det) > 0.01f);
	float a = ((p[1][1] - p[2][1]) * (x - p[2][0]) + (p[2][0] - p[1][0]) * (y - p[2][1])) / det;
	float b = ((p[2][1] - p[0][1]) * (x - p[2][0]) + (p[0][0] - p[2][0]) * (y - p[2][1])) / det;
	float c = 1 - a - b;
	assert (a >= 0 && b >= 0 && c >= 0);
	return a * p[0][2] + b * p[1][2] + c * p[2][2];
}
static void TestKnife (void)
{
	const vec3_t origin = {0, 0, 32};
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	crossing_faces = clipping_face = false; coincident_faces = sector_document = true;
	count = 2; selected = -1; knife_cuts = 0;
	Click ('k', true); Click ('k', false);
	Mouse (200, 248); polygons = 0; EditorUI_Draw (NULL); int with_hover = polygons;
	Mouse (0, 0); polygons = 0; EditorUI_Draw (NULL); assert (with_hover == polygons + 2);
	FaceClick (200, 248); assert (selected == 0 && knife_cuts == 0);
	assert (EditorUI_CancelKnife ()); assert (!EditorUI_CancelKnife ());
	FaceClick (200, 248); FaceClick (440, 248);
	assert (knife_cuts == 1 && knife_sector == 0 && knife_face == 0);
	assert (knife_start[0] == 128 && knife_end[0] == 128);
	assert (knife_start[1] == 64 && knife_end[1] == -64);
	assert (knife_start[2] == 80 && knife_end[2] == 80);
	/* A failed cut keeps the start point so the user can try another edge. */
	FaceClick (200, 248); knife_fail = true; FaceClick (440, 248);
	assert (knife_cuts == 2); EditorUI_Draw (NULL); assert (!strcmp (footer_status, "Knife validation error"));
	knife_fail = false; FaceClick (440, 248); assert (knife_cuts == 3);
	/* A changed document invalidates pending geometry. */
	FaceClick (200, 248); ++sector_revision; EditorUI_Update ();
	assert (!EditorUI_CancelKnife ()); FaceClick (440, 248); assert (knife_cuts == 3);
	EditorUI_CancelDrag (); assert (!EditorUI_CancelKnife ());
	coincident_faces = sector_document = false; session_status = "Ready";
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
}
static void TestPerspectiveHandedness (void)
{
	const vec3_t origin = {0, 0, 32};
	double old_time = realtime;
	sector_document = false; count = 0; selected = -1;
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	/* At yaw zero Quake looks along +X, with -Y on the right. */
	realtime = 20; Mouse (320, 248); Click ('d', true);
	realtime += 0.1; EditorUI_Update (); Click ('d', false); EditorUI_Draw (NULL);
	assert (camera_y < 0);
	/* Moving the mouse right turns toward that same side. */
	EditorUI_Focus (origin); Mouse (320, 248); Click (K_MOUSE2, true);
	Mouse (360, 248); Click (K_MOUSE2, false); Click ('w', true);
	realtime += 0.1; EditorUI_Update (); Click ('w', false); EditorUI_Draw (NULL);
	assert (camera_x > 0 && camera_y < 0);
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	realtime = old_time;
}
static qboolean PortalLineExists (float x0, float y0, float x1, float y1)
{
	for (int i = 0; i < outline_lines && i < 64; ++i)
	{
		const float *s = portal_outline_segments[i];
		if ((fabsf (s[0]-x0)<1 && fabsf (s[1]-y0)<1 && fabsf (s[2]-x1)<1 && fabsf (s[3]-y1)<1) ||
		    (fabsf (s[2]-x0)<1 && fabsf (s[3]-y0)<1 && fabsf (s[0]-x1)<1 && fabsf (s[1]-y1)<1)) return true;
	}
	return false;
}
static void TestPortalOutlines (void)
{
	const vec3_t origin = {0, 0, 32};
	EditorUI_SetActive (false); EditorUI_Focus (origin); EditorUI_SetActive (true);
	sector_document = true; count = 1; selected = -1; outline_fixture = 3;
	/* Only interior seams of matching fragments of the same portal disappear.
	 * Different portal destinations and noncoplanar edges must keep their
	 * lines. Fragment provenance may differ on the two portal sides; no
	 * source group or a different source group should not show a seam until
	 * the face is explicitly selected. A partially overlapping edge keeps its
	 * exposed portion, and a second opening retains its own outline. */
	static const struct { int mode, lines; } cases[] = {{1,4},{2,4},{3,4},{4,6},{6,6},{7,5},{8,8},{11,6}};
	portal_outline_horizontal = false;
	for (size_t i = 0; i < sizeof (cases) / sizeof (cases[0]); ++i)
	{
		portal_outline_case = cases[i].mode; outline_lines = outline_fills = 0;
		++sector_revision; EditorUI_Draw (NULL);
		if (outline_fills || outline_lines != cases[i].lines) fprintf (stderr, "perspective portal case %d: fills %d, lines %d expected %d\n", cases[i].mode, outline_fills, outline_lines, cases[i].lines);
		assert (outline_fills == 0 && outline_lines == cases[i].lines);
		if (cases[i].mode == 7)
		{
			/* Long edge A--B overlaps the other face only from A to M.
			 * Preserve M--B exactly, and never draw the covered A--M half. */
			const float x_a = 385, x_mid = 320, x_b = 255, y = 288.625f;
			assert (PortalLineExists (x_mid, y, x_b, y));
			assert (!PortalLineExists (x_a, y, x_mid, y));
		}
	}
	/* Selecting a volume must not restore the portal seam. Explicitly
	 * selecting a face, however, exposes its complete editable boundary. */
	portal_outline_case = 2; ++sector_revision; selected = 0; outline_lines = outline_fills = 0;
	EditorUI_Draw (NULL); assert (outline_lines == 4);
	selected = -1; Click ('f', true); Click ('f', false); FaceClick (320, 280);
	assert (selected == 0); outline_lines = outline_fills = 0;
	EditorUI_Draw (NULL); assert (outline_lines == 5);
	Click ('f', true); Click ('f', false); selected = -1;
	/* No selected sector: the screenshot's vertex-mode view should still
	 * suppress the interior portal seam. */
	portal_outline_case = 8; ++sector_revision; Click ('v', true); Click ('v', false);
	outline_lines = outline_fills = 0; EditorUI_Draw (NULL); assert (outline_lines == 8);
	Click ('v', true); Click ('v', false);
	/* Opposite portal sides can be partitioned along different diagonals;
	 * neither side may leave an internal line visible. */
	portal_outline_case = 9; ++sector_revision; count = 2; portal_outline_diagonals = outline_lines = 0;
	EditorUI_Draw (NULL); assert (outline_lines >= 4 && portal_outline_diagonals == 0);
	count = 1;
	Click (K_TAB, true); Click (K_TAB, false); portal_outline_horizontal = true;
	for (size_t i = 0; i < sizeof (cases) / sizeof (cases[0]); ++i)
	{
		portal_outline_case = cases[i].mode; outline_lines = outline_fills = 0;
		++sector_revision; EditorUI_Draw (NULL);
		if (outline_fills || outline_lines != cases[i].lines) fprintf (stderr, "top portal case %d: fills %d, lines %d expected %d\n", cases[i].mode, outline_fills, outline_lines, cases[i].lines);
		assert (outline_fills == 0 && outline_lines == cases[i].lines);
		if (cases[i].mode == 7)
		{
			float xm, ym, xa, ya, xb, yb;
			EditorUI_WorldToScreen (108, 0, &xm, &ym); EditorUI_WorldToScreen (108, -32, &xa, &ya);
			EditorUI_WorldToScreen (108, 32, &xb, &yb);
			assert (PortalLineExists (xm, ym, xb, yb));
			assert (!PortalLineExists (xa, ya, xm, ym));
		}
	}
	portal_outline_case = 9; ++sector_revision; count = 2; portal_outline_diagonals = outline_lines = 0;
	EditorUI_Draw (NULL); assert (outline_lines >= 4 && portal_outline_diagonals == 0);
	count = 1;
	Click ('4', true); Click ('4', false);
	/* Close, parallel but distinct boundaries must survive at large world
	 * coordinates. A genuine shared edge in the same location still hides. */
	const vec3_t far_origin = {1000000, 1000000, 1000032};
	portal_outline_horizontal = false; portal_outline_far = true; EditorUI_Focus (far_origin);
	portal_outline_case = 1; ++sector_revision; outline_lines = outline_fills = 0;
	EditorUI_Draw (NULL); assert (outline_lines == 4);
	portal_outline_case = 10; ++sector_revision; outline_lines = outline_fills = 0;
	EditorUI_Draw (NULL); assert (outline_lines == 6);
	portal_outline_far = false; EditorUI_Focus (origin);
	portal_outline_horizontal = false; portal_outline_case = 0; outline_fixture = 0;
	sector_document = false; selected = -1;
}
static void TestSectorProps (editor_ui_adapter_t *base)
{
	editor_ui_adapter_t props = *base;
	props.sector_prop_type = PropType; props.sector_set_prop_type = SetPropType; props.sector_create_prop = CreateProp;
	vec3_t origin = {0, 0, 32};
	EditorUI_SetActive (false); EditorUI_SetAdapter (&props); EditorUI_Focus (origin); EditorUI_SetActive (true);
	sector_document = true; count = 1; selected = 0; test_prop_type = created_props = 0; session_status = "Ready";
	Click ('o', true); Click ('o', false); assert (test_prop_type == 1);
	reject_prop_type = true; Click ('o', true); Click ('o', false); EditorUI_Draw (NULL);
	assert (test_prop_type == 1 && !strcmp (footer_status, "Detailed prop error"));
	reject_prop_type = false; session_status = "Ready";
	Click ('o', true); Click ('o', false); assert (test_prop_type == 2);
	Click ('o', true); Click ('o', false); assert (test_prop_type == 3);
	Click ('o', true); Click ('o', false); assert (test_prop_type == 0);
	/* Both viewports must pass the selected creation type rather than
	 * first creating an invalid world sector inside the room. */
	Click ('g', true); Click ('g', false); count = 0; selected = -1;
	Click (K_SHIFT, true); Mouse (350, 380); Click (K_MOUSE1, true); Mouse (430, 400); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (created_props == 1 && created_prop_type == 1);
	Click ('g', true); Click ('g', false); Click (K_TAB, true); Click (K_TAB, false); count = 0; selected = -1;
	Click (K_SHIFT, true); Mouse (320, 248); Click (K_MOUSE1, true); Mouse (352, 216); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (created_props == 2 && created_prop_type == 2);
	Click ('o', true); Click ('o', false); assert (test_prop_type == 1);
	Click ('g', true); Click ('g', false); count = 0; selected = -1;
	Click (K_SHIFT, true); Mouse (320, 248); Click (K_MOUSE1, true); Mouse (352, 216); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (created_props == 3 && created_prop_type == 3);
	Click ('g', true); Click ('g', false); count = 0; selected = -1;
	Click (K_SHIFT, true); Mouse (320, 248); Click (K_MOUSE1, true); Mouse (352, 216); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (created_props == 3 && count == 1);
	Click ('4', true); Click ('4', false);
	sector_document = false; count = 0; selected = -1;
	EditorUI_SetActive (false); EditorUI_SetAdapter (base); EditorUI_Focus (origin); EditorUI_SetActive (true);
}
static void TestCoincidentPreview (editor_ui_adapter_t *base)
{
	const vec3_t origin = {0, 0, 32};
	editor_ui_adapter_t exact = *base;
	exact.sector_vertex_coincident = CoincidentVertex;
	exact.sector_face_vertex_coincident = CoincidentFaceVertex;
	EditorUI_SetActive (false); EditorUI_SetAdapter (&exact); EditorUI_Focus (origin); EditorUI_SetActive (true);
	sector_document = coincident_preview_scene = true; count = 3; selected = 0;
	int old_moves = vertex_moves, old_merges = vertex_merges;
	Click ('v', true); Click ('v', false);
	Mouse (353, 281); Click (K_MOUSE1, true); Mouse (385, 281);
	coincident_preview_fills = coincident_preview_source_checks = coincident_preview_display_checks = 0;
	EditorUI_Draw (NULL);
	assert (coincident_preview_fills == 3 && coincident_preview_display_checks > 0);
	/* Both exact peers move together, while sector 2's distinct double value
	 * (which rounds to the same displayed float) remains at the old position. */
	assert (fabsf (coincident_preview_x[0] - coincident_preview_x[1]) < 0.01f);
	assert (coincident_preview_x[0] - coincident_preview_x[2] > 20.0f);
	/* Portal source-face outlines use the same preview as display triangles. */
	coincident_preview_portal = true; ++sector_revision; coincident_preview_source_checks = 0;
	EditorUI_Draw (NULL); assert (coincident_preview_source_checks > 0);
	coincident_preview_portal = false; ++sector_revision;
	/* Returning to the starting pixel must not snap to the float-alias target
	 * and weld or change the document. */
	Mouse (353, 281); Click (K_MOUSE1, false);
	assert (vertex_moves == old_moves && vertex_merges == old_merges);
	Mouse (353, 281); Click (K_MOUSE1, true); Mouse (385, 281); Click (K_MOUSE1, false);
	assert (vertex_moves == old_moves + 1 && vertex_merges == old_merges && moved_vertex[1] == -32);
	coincident_preview_scene = sector_document = false; selected = -1;
	EditorUI_SetActive (false); EditorUI_SetAdapter (base); EditorUI_Focus (origin); EditorUI_SetActive (true);
}
static void TestPointEntities (editor_ui_adapter_t *base)
{
	const vec3_t origin = {0,0,32}; editor_ui_adapter_t points = *base;
	points.select = SelectWithPoints; points.point_count = PointCount; points.point_origin = PointOrigin;
	points.point_selected = PointSelected; points.point_select = PointSelect; points.point_create_light = PointCreate;
	points.point_set_origin = PointMove; points.point_remove = PointRemove; points.duplicate_selected = DuplicateSelected;
	EditorUI_SetActive (false); EditorUI_SetAdapter (&points); EditorUI_Focus (origin); EditorUI_SetActive (true);
	sector_document = true; count = 1; selected = -1; test_point_selected = -1; test_point_count = 0;
	brush_mins[0] = 128; brush_mins[1] = -64; brush_mins[2] = 0;
	brush_maxs[0] = 160; brush_maxs[1] = 64; brush_maxs[2] = 160;
	Mouse (320,248); Click ('h', true); Click ('h', false);
	assert (test_point_count == 1 && test_point_selected == 0 && selected == -1);
	assert (test_points[0][0] == 112 && test_points[0][1] == 0 && test_points[0][2] == 80);
	Click (']', true); Click (']', false); assert (test_points[0][2] == 96 && test_point_moves == 1);
	/* The visible marker is selectable and drags in the 3D view plane. */
	PointSelect (NULL, -1); Mouse (320,211); Click (K_MOUSE1, true); assert (test_point_selected == 0);
	Mouse (357,211); Click (K_MOUSE1, false); assert (test_point_moves == 2 && test_points[0][1] == -16);
	Click (K_CTRL, true); Click ('d', true); Click ('d', false); Click (K_CTRL, false);
	assert (test_point_duplicates == 1 && test_point_count == 2 && test_point_selected == 1);
	/* Top-view light dragging uses world XY; a changed document cancels the drag. */
	Click (K_TAB, true); Click (K_TAB, false);
	Mouse (432,264); Click (K_MOUSE1, true); assert (test_point_selected == 0);
	Mouse (448,264); Click (K_MOUSE1, false); assert (test_point_moves == 3 && test_points[0][0] == 128);
	Mouse (448,264); Click (K_MOUSE1, true); Mouse (464,264); ++sector_revision; EditorUI_Update ();
	Click (K_MOUSE1, false); assert (test_point_moves == 3);
	Click (K_DEL, true); Click (K_DEL, false); assert (test_point_removals == 1);
	Click ('4', true); Click ('4', false);
	/* An entity hidden behind a solid face cannot intercept selection. */
	test_point_count = 1; test_points[0][0] = 200; test_points[0][1] = 0; test_points[0][2] = 80;
	test_point_selected = -1; selected = -1; Mouse (320,248); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (selected == 0 && test_point_selected == -1);
	/* On oblique intersecting surfaces, a snapped light stays on the camera
	 * side of the visible walls rather than landing in solid geometry. */
	crossing_faces = true; count = 2; selected = -1; test_point_count = 0;
	Mouse (320,248); Click ('h', true); Click ('h', false); assert (test_point_count == 1);
	for (int f = 0; f < 2; ++f)
	{
		vec3_t tri[3], e1, e2, normal, camdelta, pdelta;
		assert (FaceVertices (NULL, f, 0, tri, 3) == 3);
		VectorSubtract (tri[1], tri[0], e1); VectorSubtract (tri[2], tri[0], e2); CrossProduct (e1, e2, normal);
		vec3_t camera = {0,0,80}; VectorSubtract (camera, tri[0], camdelta); VectorSubtract (test_points[0], tri[0], pdelta);
		assert (DotProduct (normal, camdelta) * DotProduct (normal, pdelta) > 0);
	}
	crossing_faces = sector_document = false; count = 0; selected = -1; test_point_selected = -1;
	EditorUI_SetActive (false); EditorUI_SetAdapter (base); EditorUI_Focus (origin); EditorUI_SetActive (true);
}
static void ModelFieldText(int y,const char *text)
{
	Mouse(1260,y);Click(K_MOUSE1,true);Click(K_MOUSE1,false);
	Click(K_HOME,true);Click(K_HOME,false);
	for(unsigned i=0;i<256;++i){Click(K_DEL,true);Click(K_DEL,false);}
	assert(EditorControls_TextInput(text));Click(K_ENTER,true);Click(K_ENTER,false);EditorUI_Update();
}
static void LightFieldText (int y, const char *text, qboolean submit)
{
	Mouse(1200,y);Click(K_MOUSE1,true);Click(K_MOUSE1,false);
	Click(K_HOME,true);Click(K_HOME,false);
	for(unsigned i=0;i<64;++i){Click(K_DEL,true);Click(K_DEL,false);}
	assert(EditorControls_TextInput(text));
	if(submit){Click(K_ENTER,true);Click(K_ENTER,false);EditorUI_Update();}
}
static void TestLightProperties (editor_ui_adapter_t *base)
{
	editor_ui_adapter_t lights=*base;const vec3_t origin={0,0,32};
	lights.select=SelectWithPoints;lights.point_count=PointCount;lights.point_origin=PointOrigin;
	lights.point_selected=PointSelected;lights.point_select=PointSelect;
	lights.point_light_color=LightColor;lights.point_set_light_color=SetLightColor;
	for(unsigned mode=0;mode<2;++mode)
	{
		EditorUI_SetActive(false);glwidth=1280;glheight=720;EditorUI_SetAdapter(&lights);EditorUI_Focus(origin);
		sector_document=mode;count=0;selected=-1;test_point_count=2;test_point_selected=0;
		test_is_model[0]=test_is_model[1]=false;test_light_changes=test_light_swatches=0;test_light_reject=false;
		for(unsigned i=0;i<2;++i)for(unsigned a=0;a<3;++a)test_light_colors[i][a]=1;
		EditorUI_SetActive(true);EditorUI_Update();
		editor_inspector_state_t selection;EditorUI_GetInspectorState(&selection);
		assert(selection.kind==EDITOR_INSPECT_LIGHT && selection.light_color_supported && selection.light_color[0]==1);
		/* Actual retained controls: each channel applies on Enter, without
		 * overwriting the other channels or moving/clearing the point. */
		LightFieldText(112,"1",true);LightFieldText(138,"64",true);LightFieldText(164,"255",true);
		assert(test_light_changes==3 && test_point_selected==0);
		assert(test_light_colors[0][0]==1.0/255 && test_light_colors[0][1]==64.0/255 && test_light_colors[0][2]==1);
		EditorControls_Draw((cb_context_t *)(uintptr_t)1);assert(test_light_swatches==1);
		const char *invalid[]={"nan","256","-1","20 junk"};
		for(unsigned i=0;i<countof(invalid);++i)LightFieldText(112,invalid[i],true);
		assert(test_light_changes==3 && test_light_colors[0][0]==1.0/255);
		LightFieldText(112,"128",false);++sector_revision;
		Click(K_ENTER,true);Click(K_ENTER,false);EditorUI_Update();assert(test_light_changes==3);
		/* Leaving the selection cancels focus. A stale snapshot cannot edit
		 * another light even when its document revision happens to match. */
		EditorUI_GetInspectorState(&selection);test_point_selected=1;EditorUI_Update();
		assert(!EditorControls_WantsText());
		const double stale[3]={0,0,0};assert(!EditorUI_SetInspectorLightColor(&selection,stale));
		assert(test_light_changes==3 && test_light_colors[1][0]==1);
		LightFieldText(112,"0",true);assert(test_light_changes==4 && test_light_colors[1][0]==0);
		test_light_reject=true;LightFieldText(138,"0",true);assert(test_light_changes==4 && test_light_colors[1][1]==1);
		test_light_reject=false;
		LightFieldText(112,"0",true);LightFieldText(138,"0",true);LightFieldText(164,"0",true);
		assert(test_light_changes==7 && test_light_colors[1][0]==0 && test_light_colors[1][1]==0 && test_light_colors[1][2]==0);
	}
	EditorUI_SetActive(false);glwidth=640;glheight=480;test_point_count=0;test_point_selected=-1;selected=-1;count=0;
	sector_document=false;test_light_reject=false;EditorUI_SetAdapter(base);EditorUI_Focus(origin);EditorUI_SetActive(true);
}
static void TestModelProperties(editor_ui_adapter_t *base)
{
	editor_ui_adapter_t models=*base;const vec3_t origin={0,0,32};
	models.select=SelectWithPoints;models.point_count=PointCount;models.point_origin=PointOrigin;
	models.point_selected=PointSelected;models.point_select=PointSelect;models.point_set_origin=PointMove;
	models.point_remove=PointRemove;models.duplicate_selected=DuplicateSelected;
	models.point_model=ModelGet;models.point_set_model=ModelSet;models.point_create_model=ModelCreate;models.point_model_bounds=ModelBounds;
	EditorUI_SetActive(false);glwidth=1280;glheight=720;EditorUI_SetAdapter(&models);EditorUI_Focus(origin);
	count=0;selected=-1;test_point_count=0;test_point_selected=-1;test_model_updates=0;test_model_bounds_reads=0;test_model_labels=0;
	EditorUI_SetActive(true);EditorUI_Update();
	/* Real retained controls: enter a path and click Add, without console use. */
	Mouse(1260,154);Click(K_MOUSE1,true);Click(K_MOUSE1,false);assert(EditorControls_TextInput("sponza.md5mesh"));
	Mouse(1170,179);Click(K_MOUSE1,true);Click(K_MOUSE1,false);EditorUI_Update();
	assert(test_point_count==1 && test_is_model[0] && !strcmp(test_models[0].model,"progs/sponza.md5mesh"));
	editor_inspector_state_t selection;EditorUI_GetInspectorState(&selection);
	assert(selection.kind==EDITOR_INSPECT_MODEL && selection.model_supported && selection.collision_enabled);
	ModelFieldText(263,"24 -32 96");assert(test_points[0][0]==24 && test_points[0][1]==-32 && test_points[0][2]==96);
	ModelFieldText(303,"-15 90 10");assert(test_models[0].angles[0]==-15 && test_models[0].angles[1]==90);
	ModelFieldText(343,"1.25");assert(test_models[0].modelscale==1.25);
	ModelFieldText(383,"2");assert(test_models[0].skin==2);
	ModelFieldText(222,"progs/other.md3");assert(!strcmp(test_models[0].model,"progs/other.md3"));
	Mouse(1170,105);Click(K_MOUSE1,true);Click(K_MOUSE1,false);assert(!test_models[0].collision);
	/* Stale inspector snapshots cannot change a newly revised document. */
	EditorUI_GetInspectorState(&selection);++sector_revision;
	editor_map_model_t stale=selection.model;stale.skin=8;
	assert(!EditorUI_SetInspectorModel(&selection,&stale) && test_models[0].skin==2);
	EditorUI_Update();EditorUI_Draw(NULL);assert(test_model_bounds_reads && test_model_labels);
	EditorUI_SetView(EDITOR_VIEW_TOP);EditorUI_Update();EditorUI_Draw(NULL);assert(test_model_bounds_reads>=2 && test_model_labels>=2);
	/* The model point continues through shared move/duplicate/delete input. */
	int old_duplicates=test_point_duplicates;Click(K_CTRL,true);Click('d',true);Click('d',false);Click(K_CTRL,false);
	assert(test_point_duplicates==old_duplicates+1 && test_point_count==2 && test_is_model[1]);
	assert(test_models[1].modelscale==1.25 && test_models[1].skin==2);
	int old_removed=test_point_removals;Click(K_DEL,true);Click(K_DEL,false);assert(test_point_removals==old_removed+1);
	EditorUI_SetActive(false);glwidth=640;glheight=480;test_point_count=0;test_point_selected=-1;selected=-1;count=0;
	memset(test_is_model,0,sizeof(test_is_model));EditorUI_SetAdapter(base);EditorUI_Focus(origin);EditorUI_SetView(EDITOR_VIEW_PERSPECTIVE);EditorUI_SetActive(true);
}
int main (void)
{
	editor_ui_adapter_t api = {0};
	api.brush_count = BrushCount; api.brush_bounds = Bounds; api.selected = Selected; api.select = Select;
	api.brush_face_count = FaceCount; api.brush_face_vertices = FaceVertices;
	api.create_box = Create; api.set_bounds = SetBounds; api.remove_selected = Remove; api.place_spawn = Place; api.action = Action; api.status = SessionStatus;
	api.sector_mode = SectorMode; api.sector_revision = SectorRevision; api.sector_face_id = SectorFaceId; api.sector_face_is_portal = FacePortal;
	api.sector_face_portal_peer = SectorFacePortalPeer;
	api.sector_move_face = MoveFace; api.sector_vertex_count = SectorVertexCount; api.sector_vertex = SectorVertex;
	api.sector_set_vertex = SetVertex; api.sector_link_portal = LinkPortal; api.sector_unlink_portal = UnlinkPortal;
	api.sector_merge_vertex = MergeVertex;
	api.sector_same_vertex = SameVertex;
	api.sector_source_face_vertices = SourceFaceVertices; api.sector_knife = KnifeCut;
	api.sector_apply_material = ApplyMaterial;
	EditorUI_SetAdapter (&api); EditorUI_SetActive (true);
	/* Crossing faces cannot be sorted globally: each is closer at one side.
	 * Check the actual projected depths supplied to the rasterizer. */
	crossing_faces = true; crossing_count = crossing_outlines = 0; count = 2; selected = 0;
	EditorUI_Draw (NULL);
	assert (crossing_count == 2 && crossing_outlines == 3 && depth_clears == 1);
	/* At yaw zero the point with negative Y must project to screen right. */
	assert (crossing_polygons[0][0][0] > glwidth * 0.5f);
	assert (DepthAt (crossing_polygons[0], 270, 300) < DepthAt (crossing_polygons[1], 270, 300));
	assert (DepthAt (crossing_polygons[0], 370, 300) > DepthAt (crossing_polygons[1], 370, 300));
	/* Face mode selects 3D polygons, cycles coincident ray hits, and links
	 * two explicitly selected portal faces without changing legacy shortcuts. */
	sector_document = true; selected = -1;
	Click ('f', true); Click ('f', false);
	Mouse (320, 248); Click (K_MOUSE1, true); Click (K_MOUSE1, false); assert (selected == 0);
	Click ('l', true); Click ('l', false);
	Click (K_CTRL, true); Click (K_MOUSE1, true); Click (K_MOUSE1, false); Click (K_CTRL, false); assert (selected == 1);
	fail_portal = true; Click ('l', true); Click ('l', false);
	crossing_count = crossing_outlines = 0; EditorUI_Draw (NULL);
	assert (!strcmp (footer_status, "Detailed portal validation error"));
	fail_portal = false;
	Click ('l', true); Click ('l', false); assert (portal_links == 1);
	Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	Click (K_CTRL, true); Click (K_MOUSE1, true); Click (K_MOUSE1, false); Click (K_CTRL, false); assert (selected == 1);
	Click ('u', true); Click ('u', false); assert (portal_unlinks == 1);
	Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	Click (K_CTRL, true); Click (K_MOUSE1, true); Click (K_MOUSE1, false); Click (K_CTRL, false); assert (selected == 1);
	Click ('t', true); Click ('t', false); assert (materials == 1);
	/* A document mutation invalidates the remembered portal source. */
	Click ('l', true); Click ('l', false); ++sector_revision; EditorUI_Update ();
	Click (K_CTRL, true); Click (K_MOUSE1, true); Click (K_MOUSE1, false); Click (K_CTRL, false); assert (selected == 0);
	Click ('l', true); Click ('l', false); assert (portal_links == 1);
	Click ('l', true); Click ('l', false); /* Cancel the newly marked source. */
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (320, 200); Click (K_MOUSE1, false);
	assert (face_moves == 1 && DotProduct (moved_face_delta, moved_face_delta) > 0);
	Click ('v', true); Click ('v', false);
	Mouse (385, 313); Click (K_MOUSE1, true); Mouse (360, 313); Click (K_MOUSE1, false);
	assert (vertex_moves == 1 && moved_vertex[1] != -25);
	Click ('v', true); Click ('v', false); sector_document = false;
	TestFaceCycling ();
	TestVertexSnapping ();
	TestKnife ();
	TestPerspectiveHandedness ();
	crossing_faces = false; selected = -1;
	/* A quad rendered as two triangles must have four outlines, while two
	 * authored triangular faces retain their shared (knife) edge. */
	sector_document = true; count = 1; selected = 0;
	outline_fixture = 1; outline_lines = outline_fills = 0; EditorUI_Draw (NULL);
	assert (outline_fills == 2 && outline_lines == 4);
	outline_fixture = 2; outline_lines = outline_fills = 0; EditorUI_Draw (NULL);
	assert (outline_fills == 2 && outline_lines == 6);
	Click (K_TAB, true); Click (K_TAB, false);
	outline_fixture = 1; outline_lines = outline_fills = 0; EditorUI_Draw (NULL);
	assert (outline_fills == 0 && outline_lines == 2);
	outline_fixture = 2; outline_lines = outline_fills = 0; EditorUI_Draw (NULL);
	assert (outline_fills == 0 && outline_lines == 4);
	Click ('4', true); Click ('4', false);
	outline_fixture = 0; sector_document = false; selected = -1;
	TestPortalOutlines ();
	TestCoincidentPreview (&api);
	TestSectorProps (&api);
	TestPointEntities (&api);
	TestModelProperties (&api);
	TestLightProperties (&api);
	/* The clipping intersection must interpolate reciprocal depth, too. */
	clipping_face = true; clipped_count = 0; count = 1;
	EditorUI_Draw (NULL);
	assert (clipped_count == 4);
	qboolean found_clipped_edge = false;
	for (int i = 0; i < clipped_count; ++i)
		if (fabsf (clipped_polygon[i][0] - glwidth) < 0.001f && fabsf (clipped_polygon[i][2] - (0.01f - 0.005f * 200.0f / 520.0f)) < 0.000001f)
			found_clipped_edge = true;
	assert (found_clipped_edge);
	clipping_face = false;
	EditorUI_SetStatus ("Local help"); EditorUI_Draw (NULL); assert (!strcmp (footer_status, "Local help"));
	session_status = "A newer session error"; EditorUI_Draw (NULL); assert (!strcmp (footer_status, "A newer session error"));
	EditorUI_SetStatus ("Temporary help"); realtime += 5.1; EditorUI_Draw (NULL); assert (!strcmp (footer_status, "A newer session error"));
	/* Perspective mode renders and picks real face polygons. */
	count = 1; brush_mins[0] = 128; brush_mins[1] = -16; brush_mins[2] = 64;
	brush_maxs[0] = 160; brush_maxs[1] = 16; brush_maxs[2] = 96;
	Mouse (320, 248); EditorUI_Draw (NULL); assert (polygons > 0);
	Click (K_MOUSE1, true); assert (selected == 0); Mouse (360, 248); Click (K_MOUSE1, false);
	assert (brush_mins[1] != -16);
	/* Whole-volume movement also uses absolute grid snapping, but preserves
	 * an unmoved off-grid axis and doesn't move on click. */
	brush_mins[0] = 130; brush_maxs[0] = 162; brush_mins[1] = -25; brush_maxs[1] = 7;
	Mouse (320, 248); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
	assert (brush_mins[0] == 130 && brush_mins[1] == -25);
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (288, 248); Click (K_MOUSE1, false);
	assert (brush_mins[0] == 130 && brush_mins[1] == -16 && brush_maxs[1] == 16);
	/* A ray through the empty corner of a tetrahedron's bounding box must miss. */
	tetrahedron = true; selected = -1; Mouse (300, 228); Click (K_MOUSE1, true); Click (K_MOUSE1, false); assert (selected == -1);
	Mouse (340, 268); Click (K_MOUSE1, true); Click (K_MOUSE1, false); assert (selected == 0); tetrahedron = false;
	/* A perspective drag on the ground plane constructs a real box. */
	count = 0; selected = -1; Click (K_SHIFT, true);
	Mouse (350, 380); Click (K_MOUSE1, true); Mouse (430, 400); Click (K_MOUSE1, false); Click (K_SHIFT, false);
	assert (count == 1 && brush_mins[0] < brush_maxs[0] && brush_mins[1] < brush_maxs[1]);
	assert (brush_mins[2] == 0 && brush_maxs[2] == 64);
	/* Releasing a movement key with Ctrl held must stop the camera. */
	realtime = 1; EditorUI_Update (); Click ('s', true); realtime = 1.05; EditorUI_Update ();
	Click (K_CTRL, true); Click ('s', false); EditorUI_Draw (NULL); float stopped_x = camera_x;
	realtime = 1.1; EditorUI_Update (); EditorUI_Draw (NULL); assert (camera_x == stopped_x);
	Click (K_CTRL, false);
	vec3_t focus = {700, 100, 32}; EditorUI_Focus (focus); EditorUI_Draw (NULL); assert (camera_x == 700);
	focus[0] = focus[1] = 0; EditorUI_Focus (focus);
	Click ('w', true);
	for (int i = 0; i < 10; ++i) { realtime += 0.1; EditorUI_Update (); }
	Click ('w', false); EditorUI_Draw (NULL); assert (camera_x > 250);
	EditorUI_Focus (focus);
	count = 0; selected = -1; Click (K_TAB, true); Click (K_TAB, false);
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (352, 216); Click (K_MOUSE1, false);
	assert (count == 1 && brush_mins[0] == 0 && brush_mins[1] == 0 && brush_mins[2] == -64);
	assert (brush_maxs[0] == 32 && brush_maxs[1] == 32 && brush_maxs[2] == 64);
	Mouse (336, 232); Click (K_MOUSE1, true); Mouse (368, 232); Click (K_MOUSE1, false);
	assert (brush_mins[0] == 32 && brush_maxs[0] == 64);
	Mouse (384, 248); Click (K_MOUSE1, true); Mouse (400, 264); Click (K_MOUSE1, false);
	assert (brush_maxs[0] == 80 && brush_mins[1] == -16);
	{
		vec3_t saved_mins, saved_maxs; VectorCopy (brush_mins, saved_mins); VectorCopy (brush_maxs, saved_maxs);
		brush_mins[0] = 5; brush_maxs[0] = 37; brush_mins[1] = 3; brush_maxs[1] = 35;
		Mouse (335, 230); Click (K_MOUSE1, true); Click (K_MOUSE1, false);
		assert (brush_mins[0] == 5 && brush_mins[1] == 3);
		Mouse (335, 230); Click (K_MOUSE1, true); Mouse (351, 230); Click (K_MOUSE1, false);
		assert (brush_mins[0] == 16 && brush_maxs[0] == 48 && brush_mins[1] == 3);
		VectorCopy (saved_mins, brush_mins); VectorCopy (saved_maxs, brush_maxs);
	}
	Click (K_SHIFT, true); Click (']', true); Click (K_SHIFT, false);
	assert (brush_maxs[2] == 80 && brush_mins[2] == -64);
	Mouse (320, 248); Click ('p', true); assert (spawns == 1 && spawn[2] == 32);
	Click (K_CTRL, true); Click ('s', true); Click ('p', true); Click (K_CTRL, false);
	assert ((actions & (1 << EDITOR_UI_SAVE)) && (actions & (1 << EDITOR_UI_PLAY)));
	float before_x, before_y, after_x, after_y;
	Mouse (400, 200); EditorUI_ScreenToWorld (400, 200, &before_x, &before_y); Click (K_MWHEELUP, true);
	EditorUI_ScreenToWorld (400, 200, &after_x, &after_y);
	assert (fabsf (before_x - after_x) < 0.01f && fabsf (before_y - after_y) < 0.01f);
	EditorUI_Draw (NULL); Click (K_DEL, true); assert (count == 0);
	/* Opening a menu or console cancels an unfinished drag without committing. */
	Mouse (320, 248); Click (K_MOUSE1, true); Mouse (400, 200);
	EditorUI_CancelDrag (); Click (K_MOUSE1, false); assert (count == 0);
	EditorUI_SetActive (false); assert (!EditorUI_KeyEvent ('p', true));
	return 0;
}
