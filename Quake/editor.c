/* Source-map editing session and asynchronous playtest builds. */
#include "quakedef.h"
#include "editor.h"
#include "editor_map.h"
#include "editor_sector.h"
#include "editor_sector_portal.h"
#include "editor_process.h"
#include "editor_tools.h"
#include "editor_ui.h"
#include "editor_model_preview.h"
#include "static_models_engine.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <time.h>

#define EDITOR_HISTORY 32
#define EDITOR_MAX_SOURCE (32u * 1024u * 1024u)
static cvar_t editor_qbsp = {"editor_qbsp", "", CVAR_ARCHIVE};
static cvar_t editor_texture = {"editor_texture", "stone1", CVAR_ARCHIVE};
typedef enum { EDITOR_BUILD_QBSP, EDITOR_BUILD_VIS, EDITOR_BUILD_LIGHT, EDITOR_BUILD_SCRIPT } editor_build_stage_t;
typedef struct { int count, source_face; vec3_t *vertices; size_t *vertex_indices; } editor_face_geometry_t;
typedef struct
{
	int count, vertex_count;
	editor_face_geometry_t *faces;
	size_t *vertex_indices;
	vec3_t mins, maxs;
	qboolean has_bounds;
} editor_brush_geometry_t;
typedef struct
{
	char path[EDITOR_MAP_MODEL_PATH_MAX];
	vec3_t mins,maxs;
	qboolean valid;
} editor_model_bounds_t;
static struct
{
	editor_map_t map, undo[EDITOR_HISTORY], redo[EDITOR_HISTORY];
	editor_sector_map_t sectors, sector_undo[EDITOR_HISTORY], sector_redo[EDITOR_HISTORY];
	int nundo, nredo, selected, selected_point;
	int nsector_undo, nsector_redo;
	qboolean initialized, has_map, dirty, playtesting, sector_mode;
	uint64_t revision, compiled_revision, building_revision;
	char name[80], status[256], source_path[MAX_OSPATH], bsp_path[MAX_OSPATH], build_name[80];
	char script_path[256], script_package[MAX_OSPATH], script_snapshot[MAX_OSPATH];
	editor_process_t *process;
	editor_process_t *play_process;
	editor_build_stage_t build_stage;
	editor_brush_geometry_t *geometry;
	int geometry_count;
	editor_model_bounds_t *model_bounds;
	size_t model_bounds_count;
	unsigned build_serial;
	byte *static_model_lump;
	size_t static_model_lump_bytes;
} editor = {.selected = -1, .selected_point = -1};

static void Editor_RebuildGeometry (void);
static void Editor_RebuildModelBounds (void)
{
	const editor_map_t *map=editor.sector_mode?&editor.sectors.entities:&editor.map;
	editor_model_bounds_t *next=NULL;size_t count=0;
	if(map->num_entities && map->num_entities<=SIZE_MAX/sizeof(*next)) next=calloc(map->num_entities,sizeof(*next));
	if(!next && map->num_entities)return;
	for(size_t i=1;i<map->num_entities;++i)
	{
		editor_map_model_t value;if(!EditorMap_GetModel(map,i,&value))continue;
		qboolean found=false;for(size_t j=0;j<count;++j)if(!strcmp(next[j].path,value.model)){found=true;break;}
		if(found)continue;
		editor_model_bounds_t *bounds=&next[count++];q_strlcpy(bounds->path,value.model,sizeof(bounds->path));
		/* This runs only on document edits/builds. Mod_ForName reuses loaded
		 * assets; refreshing the numeric bounds also honors explicit reloads. */
		qmodel_t *model=Mod_ForName(value.model,false);
		if(!model || model->type!=mod_alias)continue;
		bounds->valid=true;
		for(unsigned a=0;a<3;++a)
		{
			if(!isfinite(model->mins[a]) || !isfinite(model->maxs[a]) || model->mins[a]>model->maxs[a])bounds->valid=false;
			bounds->mins[a]=model->mins[a];bounds->maxs[a]=model->maxs[a];
		}
	}
	free(editor.model_bounds);editor.model_bounds=next;editor.model_bounds_count=count;
}
static void Editor_FreeGeometry (void)
{
	for (int i = 0; i < editor.geometry_count; ++i)
	{
		for (int f = 0; f < editor.geometry[i].count; ++f)
		{
			free (editor.geometry[i].faces[f].vertices);
			free (editor.geometry[i].faces[f].vertex_indices);
		}
		free (editor.geometry[i].faces);
		free (editor.geometry[i].vertex_indices);
	}
	free (editor.geometry);
	editor.geometry = NULL;
	editor.geometry_count = 0;
}

static void Editor_Status (const char *message)
{
	q_strlcpy (editor.status, message, sizeof (editor.status));
	Con_Printf ("Editor: %s\n", editor.status);
}
static qboolean Editor_ValidName (const char *name)
{
	if (!name || !*name || strlen (name) >= sizeof (editor.name)) return false;
	for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return false;
	return true;
}
static qboolean Editor_MapPath (const char *name, const char *extension, char path[MAX_OSPATH])
{
	int len = q_snprintf (path, MAX_OSPATH, "%s/maps/%s.%s", com_gamedir, name, extension);
	return len >= 0 && len < MAX_OSPATH;
}
static void Editor_FreeHistory (editor_map_t *history, int *count)
{
	while (*count) EditorMap_Free (&history[--*count]);
}
static void Editor_PushHistory (editor_map_t *history, int *count, editor_map_t *map)
{
	if (*count == EDITOR_HISTORY)
	{
		EditorMap_Free (&history[0]);
		memmove (&history[0], &history[1], (EDITOR_HISTORY - 1) * sizeof (*history));
		--*count;
	}
	history[(*count)++] = *map;
	memset (map, 0, sizeof (*map));
}
static void Editor_FreeSectorHistory (editor_sector_map_t *history, int *count)
{
	while (*count) EditorSector_Free (&history[--*count]);
}
static void Editor_PushSectorHistory (editor_sector_map_t *history, int *count, editor_sector_map_t *map)
{
	if (*count == EDITOR_HISTORY)
	{
		EditorSector_Free (&history[0]);
		memmove (&history[0], &history[1], (EDITOR_HISTORY - 1) * sizeof (*history));
		--*count;
	}
	history[(*count)++] = *map;
	memset (map, 0, sizeof (*map));
}
static void Editor_CommitSector (editor_sector_map_t *next)
{
	Editor_FreeSectorHistory (editor.sector_redo, &editor.nsector_redo);
	Editor_PushSectorHistory (editor.sector_undo, &editor.nsector_undo, &editor.sectors);
	editor.sectors = *next; memset (next, 0, sizeof (*next));
	editor.dirty = true;
	++editor.revision;
	Editor_RebuildGeometry ();
}
static qboolean Editor_CloneSector (editor_sector_map_t *copy)
{
	memset (copy, 0, sizeof (*copy));
	if (EditorSector_Clone (copy, &editor.sectors)) return true;
	Editor_Status ("Unable to copy sector map for editing");
	return false;
}
static void Editor_Commit (editor_map_t *next)
{
	Editor_FreeHistory (editor.redo, &editor.nredo);
	Editor_PushHistory (editor.undo, &editor.nundo, &editor.map);
	editor.map = *next; memset (next, 0, sizeof (*next));
	editor.dirty = true;
	++editor.revision;
	Editor_RebuildGeometry ();
}
static qboolean Editor_Clone (editor_map_t *copy)
{
	memset (copy, 0, sizeof (*copy));
	if (EditorMap_Clone (copy, &editor.map)) return true;
	Editor_Status ("Unable to copy map for editing (out of memory)");
	return false;
}
static void Editor_UndoRedo (qboolean redo)
{
	if (editor.sector_mode)
	{
		editor_sector_map_t *from = redo ? editor.sector_redo : editor.sector_undo, *to = redo ? editor.sector_undo : editor.sector_redo;
		int *nfrom = redo ? &editor.nsector_redo : &editor.nsector_undo, *nto = redo ? &editor.nsector_undo : &editor.nsector_redo;
		if (!*nfrom) { Editor_Status (redo ? "Nothing to redo" : "Nothing to undo"); return; }
		Editor_PushSectorHistory (to, nto, &editor.sectors);
		editor.sectors = from[--*nfrom]; memset (&from[*nfrom], 0, sizeof (*from));
		editor.selected = editor.selected_point = -1; editor.dirty = true; ++editor.revision;
		Editor_RebuildGeometry ();
		Editor_Status (redo ? "Redid change" : "Undid change");
		return;
	}
	editor_map_t *from = redo ? editor.redo : editor.undo, *to = redo ? editor.undo : editor.redo;
	int *nfrom = redo ? &editor.nredo : &editor.nundo, *nto = redo ? &editor.nundo : &editor.nredo;
	if (!*nfrom) { Editor_Status (redo ? "Nothing to redo" : "Nothing to undo"); return; }
	Editor_PushHistory (to, nto, &editor.map);
	editor.map = from[--*nfrom]; memset (&from[*nfrom], 0, sizeof (*from));
	editor.selected = editor.selected_point = -1; editor.dirty = true; ++editor.revision;
	Editor_RebuildGeometry ();
	Editor_Status (redo ? "Redid change" : "Undid change");
}
static editor_map_brush_t *Editor_Brush (editor_map_t *map, int index, size_t *entity_out, size_t *brush_out)
{
	if (index < 0) return NULL;
	for (size_t e = 0; e < map->num_entities; ++e)
	{
		if ((size_t)index < map->entities[e].num_brushes)
		{
			if (entity_out) *entity_out = e;
			if (brush_out) *brush_out = (size_t)index;
			return &map->entities[e].brushes[index];
		}
		index -= (int)map->entities[e].num_brushes;
	}
	return NULL;
}
static int Editor_BrushCount (void *ctx)
{
	if (editor.sector_mode) return editor.sectors.num_sectors > INT_MAX ? INT_MAX : (int)editor.sectors.num_sectors;
	(void)ctx; size_t n = 0;
	for (size_t i = 0; i < editor.map.num_entities; ++i)
	{
		if (editor.map.entities[i].num_brushes > INT_MAX - n) return INT_MAX;
		n += editor.map.entities[i].num_brushes;
	}
	return (int)n;
}
/* Construct once on the main thread after each document change. The GUI task
 * reads this immutable snapshot until the frame has joined. */
static void Editor_RebuildGeometry (void)
{
	Editor_FreeGeometry ();
	Editor_RebuildModelBounds ();
	int count = Editor_BrushCount (NULL);
	if (!count) return;
	editor.geometry = calloc ((size_t)count, sizeof (*editor.geometry));
	if (!editor.geometry) return;
	editor.geometry_count = count;
	for (int i = 0; i < count; ++i)
	{
		if (editor.sector_mode)
		{
			const editor_sector_t *sector = &editor.sectors.sectors[i];
			editor_brush_geometry_t *geometry = &editor.geometry[i];
			bool *seen = calloc (editor.sectors.num_vertices, sizeof (*seen));
			if (seen)
			{
				size_t vertex_count = 0;
				for (size_t f = 0; f < sector->num_faces; ++f)
					for (size_t v = 0; v < sector->faces[f].num_vertices; ++v)
					{
						size_t index = sector->faces[f].vertices[v];
						if (index < editor.sectors.num_vertices && !seen[index]) { seen[index] = true; ++vertex_count; }
					}
				if (vertex_count <= INT_MAX && vertex_count <= SIZE_MAX / sizeof (*geometry->vertex_indices))
					geometry->vertex_indices = malloc (vertex_count * sizeof (*geometry->vertex_indices));
				if (geometry->vertex_indices)
				{
					memset (seen, 0, editor.sectors.num_vertices * sizeof (*seen));
					for (size_t f = 0; f < sector->num_faces; ++f)
						for (size_t v = 0; v < sector->faces[f].num_vertices; ++v)
						{
							size_t index = sector->faces[f].vertices[v];
							if (index >= editor.sectors.num_vertices || seen[index]) continue;
							seen[index] = true;
							geometry->vertex_indices[geometry->vertex_count++] = index;
							for (int axis = 0; axis < 3; ++axis)
							{
								float value = (float)editor.sectors.vertices[index].position[axis];
								if (!geometry->has_bounds || value < geometry->mins[axis]) geometry->mins[axis] = value;
								if (!geometry->has_bounds || value > geometry->maxs[axis]) geometry->maxs[axis] = value;
							}
							geometry->has_bounds = true;
						}
				}
				free (seen);
			}
			size_t total = 0;
			for (size_t f = 0; f < sector->num_faces; ++f)
			{
				size_t n = EditorSector_TriangulateFace (&editor.sectors, (size_t)i, f, NULL, 0);
				if (n > INT_MAX - total) { total = 0; break; }
				total += n;
			}
			if (!total) continue;
			geometry->faces = calloc (total, sizeof (*geometry->faces));
			if (!geometry->faces) continue;
			for (size_t f = 0; f < sector->num_faces; ++f)
			{
				size_t n = EditorSector_TriangulateFace (&editor.sectors, (size_t)i, f, NULL, 0);
				if (!n || n > SIZE_MAX / sizeof (size_t[3])) continue;
				size_t (*triangles)[3] = malloc (n * sizeof (*triangles));
				if (!triangles) continue;
				if (EditorSector_TriangulateFace (&editor.sectors, (size_t)i, f, triangles, n) == n)
					for (size_t t = 0; t < n; ++t)
					{
						editor_face_geometry_t *face = &geometry->faces[geometry->count];
						face->vertices = malloc (3 * sizeof (*face->vertices));
						face->vertex_indices = malloc (3 * sizeof (*face->vertex_indices));
						if (!face->vertices || !face->vertex_indices)
						{ free (face->vertices); free (face->vertex_indices); face->vertices = NULL; face->vertex_indices = NULL; break; }
						face->count = 3; face->source_face = (int)f;
						for (int v = 0; v < 3; ++v)
						{
							face->vertex_indices[v] = triangles[t][v];
							for (int axis = 0; axis < 3; ++axis)
								face->vertices[v][axis] = (float)editor.sectors.vertices[triangles[t][v]].position[axis];
						}
						++geometry->count;
					}
				free (triangles);
			}
			continue;
		}
		const editor_map_brush_t *brush = Editor_Brush (&editor.map, i, NULL, NULL);
		if (!brush || brush->num_faces > 64) continue;
		editor_brush_geometry_t *geometry = &editor.geometry[i];
		geometry->faces = calloc (brush->num_faces, sizeof (*geometry->faces));
		if (!geometry->faces) continue;
		geometry->count = (int)brush->num_faces;
		for (int f = 0; f < geometry->count; ++f)
		{
			double points[64][3];
			size_t n = EditorMap_BrushFaceVertices (brush, (size_t)f, points, 64);
			if (!n) continue;
			editor_face_geometry_t *face = &geometry->faces[f];
			face->vertices = malloc (n * sizeof (*face->vertices));
			if (!face->vertices) continue;
			face->count = (int)n;
			face->source_face = f;
			for (int v = 0; v < face->count; ++v)
				for (int axis = 0; axis < 3; ++axis) face->vertices[v][axis] = (float)points[v][axis];
		}
	}
}
static int Editor_FaceCount (void *ctx, int index)
{
	(void)ctx;
	return index >= 0 && index < editor.geometry_count ? editor.geometry[index].count : 0;
}
static int Editor_FaceVertices (void *ctx, int index, int face, vec3_t *vertices, int capacity)
{
	(void)ctx;
	if (index < 0 || index >= editor.geometry_count || face < 0 || face >= editor.geometry[index].count || !vertices) return 0;
	const editor_face_geometry_t *geometry = &editor.geometry[index].faces[face];
	if (!geometry->count || capacity < geometry->count) return 0;
	memcpy (vertices, geometry->vertices, (size_t)geometry->count * sizeof (*vertices));
	return geometry->count;
}
static qboolean Editor_Bounds (void *ctx, int index, vec3_t mins, vec3_t maxs)
{
	if (editor.sector_mode)
	{
		if (index < 0 || (size_t)index >= editor.sectors.num_sectors) return false;
		if (index < editor.geometry_count && editor.geometry[index].has_bounds)
		{
			VectorCopy (editor.geometry[index].mins, mins); VectorCopy (editor.geometry[index].maxs, maxs);
			return true;
		}
		const editor_sector_t *sector = &editor.sectors.sectors[index];
		qboolean any = false;
		for (size_t f = 0; f < sector->num_faces; ++f)
			for (size_t v = 0; v < sector->faces[f].num_vertices; ++v)
			{
				size_t index_v = sector->faces[f].vertices[v];
				if (index_v >= editor.sectors.num_vertices) return false;
				for (int axis = 0; axis < 3; ++axis)
				{
					float value = (float)editor.sectors.vertices[index_v].position[axis];
					if (!isfinite (value)) return false;
					if (!any || value < mins[axis]) mins[axis] = value;
					if (!any || value > maxs[axis]) maxs[axis] = value;
				}
				any = true;
			}
		return any;
	}
	(void)ctx; editor_map_brush_t *brush = Editor_Brush (&editor.map, index, NULL, NULL);
	double a[3], b[3];
	if (!brush || !EditorMap_BrushBounds (brush, a, b)) return false;
	for (int i = 0; i < 3; ++i) { mins[i] = (float)a[i]; maxs[i] = (float)b[i]; }
	return true;
}
static int Editor_Selected (void *ctx) { (void)ctx; return editor.selected; }
static void Editor_Select (void *ctx, int index) { (void)ctx; editor.selected = index; editor.selected_point = -1; }
static qboolean Editor_AddBox (void *ctx, const vec3_t mins, const vec3_t maxs)
{
	if (editor.sector_mode)
	{
		editor_sector_map_t next; double a[3], b[3]; size_t index; char error[256] = "";
		for (int i = 0; i < 3; ++i) { a[i] = mins[i]; b[i] = maxs[i]; }
		if (!Editor_CloneSector (&next)) return false;
		if (!EditorSector_AddBox (&next, a, b, editor_texture.string, &index, error, sizeof (error)))
		{
			EditorSector_Free (&next); Editor_Status (*error ? error : "Could not create sector"); return false;
		}
		Editor_CommitSector (&next); editor.selected = (int)index; editor.selected_point = -1; Editor_Status ("Sector created"); return true;
	}
	(void)ctx; editor_map_t next; double a[3], b[3];
	for (int i = 0; i < 3; ++i) { a[i] = mins[i]; b[i] = maxs[i]; }
	if (!Editor_Clone (&next)) return false;
	if (!next.num_entities || !EditorMap_AddBox (&next, 0, a, b, editor_texture.string, NULL))
	{
		EditorMap_Free (&next); Editor_Status ("Could not create brush; check editor_texture and dimensions"); return false;
	}
	Editor_Commit (&next); editor.selected = (int)editor.map.entities[0].num_brushes - 1; editor.selected_point = -1;
	Editor_Status ("Brush created"); return true;
}
static qboolean Editor_CreateProp (void *ctx, const vec3_t mins, const vec3_t maxs, int type)
{
	(void)ctx;
	if (!editor.sector_mode || type < EDITOR_SECTOR_PROP_STATIC || type > EDITOR_SECTOR_PROP_MOVABLE) return false;
	editor_sector_map_t next; double a[3], b[3]; size_t index; char error[256] = "";
	for (int i = 0; i < 3; ++i) { a[i] = mins[i]; b[i] = maxs[i]; }
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_AddBoxOfType (&next, a, b, editor_texture.string, (editor_sector_prop_type_t)type, &index, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not create prop"); return false; }
	Editor_CommitSector (&next); editor.selected = (int)index; editor.selected_point = -1; Editor_Status ("Sector prop created"); return true;
}
static editor_map_t *Editor_PointMap (void)
{
	if (!editor.has_map) return NULL;
	return editor.sector_mode ? &editor.sectors.entities : &editor.map;
}
/* Script paths are relative to the active project. Avoid absolute paths,
 * traversal and command separators before handing a path to build tools. */
static qboolean Editor_ValidScriptPath (const char *path)
{
	if (!path || strlen (path) >= 256) return false;
	if (!*path) return true;
	if (*path == '/' || strstr (path, "..") || strstr (path, "//")) return false;
	for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.' || *p == '/')) return false;
	size_t n = strlen (path);
	return n > 3 && !strcmp (path + n - 3, ".qs");
}
static const char *Editor_WorldScript (void *ctx)
{
	(void)ctx; const editor_map_t *map = Editor_PointMap ();
	const char *value = map && map->num_entities ? EditorMap_GetProperty (&map->entities[0], "_quiver_script") : NULL;
	return value ? value : "";
}
static qboolean Editor_SetWorldScript (void *ctx, const char *path)
{
	(void)ctx;
	if (!editor.has_map || !Editor_ValidScriptPath (path))
	{ Editor_Status ("Script must be a project-relative .qs path"); return false; }
	if (!strcmp (Editor_WorldScript (NULL), path)) return true;
	if (editor.sector_mode)
	{
		editor_sector_map_t next;
		if (!Editor_CloneSector (&next)) return false;
		if (!EditorMap_SetProperty (&next.entities.entities[0], "_quiver_script", path))
		{ EditorSector_Free (&next); Editor_Status ("Could not update script path"); return false; }
		Editor_CommitSector (&next);
	}
	else
	{
		editor_map_t next;
		if (!Editor_Clone (&next)) return false;
		if (!EditorMap_SetProperty (&next.entities[0], "_quiver_script", path))
		{ EditorMap_Free (&next); Editor_Status ("Could not update script path"); return false; }
		Editor_Commit (&next);
	}
	Editor_Status ("World script updated"); return true;
}
static qboolean Editor_IsLight (const editor_map_t *map, int index)
{
	if (!map || index <= 0 || (size_t)index >= map->num_entities) return false;
	const editor_map_entity_t *entity = &map->entities[index];
	const char *classname = EditorMap_GetProperty (entity, "classname");
	return !entity->num_brushes && classname && !strcmp (classname, "light");
}
static qboolean Editor_IsEditablePoint (const editor_map_t *map, int index)
{ return Editor_IsLight (map,index) || (index>=0 && EditorMap_IsModel (map,(size_t)index)); }
static int Editor_PointCount (void *ctx)
{
	(void)ctx; const editor_map_t *map = Editor_PointMap ();
	return !map ? 0 : map->num_entities > INT_MAX ? INT_MAX : (int)map->num_entities;
}
static qboolean Editor_PointOrigin (void *ctx, int index, vec3_t origin)
{
	(void)ctx; const editor_map_t *map = Editor_PointMap (); double point[3];
	if (!Editor_IsEditablePoint (map, index) || !EditorMap_GetPointOrigin (map, (size_t)index, point)) return false;
	for (int i = 0; i < 3; ++i)
	{
		if (!isfinite (point[i]) || fabs (point[i]) > EDITOR_SECTOR_COORD_LIMIT) return false;
		origin[i] = (float)point[i];
	}
	return true;
}
static int Editor_PointSelected (void *ctx) { (void)ctx; return editor.selected_point; }
static void Editor_PointSelect (void *ctx, int index)
{
	(void)ctx;
	if (index >= 0 && !Editor_IsEditablePoint (Editor_PointMap (), index)) return;
	editor.selected_point = index;
	if (index >= 0) editor.selected = -1;
}
/* Clone the correct document so point edits participate in the same undo
 * history as sector and brush edits. */
static qboolean Editor_ClonePointDocument (editor_sector_map_t *sectors, editor_map_t *map, editor_map_t **points)
{
	memset (sectors, 0, sizeof (*sectors)); memset (map, 0, sizeof (*map));
	if (editor.sector_mode) { if (!Editor_CloneSector (sectors)) return false; *points = &sectors->entities; }
	else { if (!Editor_Clone (map)) return false; *points = map; }
	return true;
}
static void Editor_FreePointDocument (editor_sector_map_t *sectors, editor_map_t *map)
{ if (editor.sector_mode) EditorSector_Free (sectors); else EditorMap_Free (map); }
static void Editor_CommitPointDocument (editor_sector_map_t *sectors, editor_map_t *map)
{ if (editor.sector_mode) Editor_CommitSector (sectors); else Editor_Commit (map); }
static qboolean Editor_CreateLight (void *ctx, const vec3_t origin)
{
	(void)ctx; if (!editor.has_map) return false;
	double point[3]; for (int i = 0; i < 3; ++i) { point[i] = origin[i]; if (!isfinite (point[i]) || fabs (point[i]) > EDITOR_SECTOR_COORD_LIMIT) return false; }
	editor_sector_map_t sectors; editor_map_t map, *points; size_t index;
	if (!Editor_ClonePointDocument (&sectors, &map, &points)) return false;
	if (!EditorMap_AddLight (points, point, &index) || index > INT_MAX)
	{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not create light"); return false; }
	Editor_CommitPointDocument (&sectors, &map); editor.selected = -1; editor.selected_point = (int)index; Editor_Status ("Light created"); return true;
}
static qboolean Editor_PointLightColor (void *ctx, int index, double rgb[3])
{ (void)ctx; return index >= 0 && EditorMap_GetLightColor (Editor_PointMap (), (size_t)index, rgb); }
static qboolean Editor_SetPointLightColor (void *ctx, int index, const double rgb[3])
{
	(void)ctx;
	double old[3];
	if (!rgb || !Editor_PointLightColor (NULL, index, old)) return false;
	for (unsigned i = 0; i < 3; ++i)
		if (!isfinite (rgb[i]) || rgb[i] < 0 || rgb[i] > 1)
		{ Editor_Status ("Light RGB channels must be between 0 and 255"); return false; }
	char canonical[192]; q_snprintf (canonical, sizeof (canonical), "%.17g %.17g %.17g", rgb[0], rgb[1], rgb[2]);
	const editor_map_entity_t *current = &Editor_PointMap ()->entities[index];
	const char *authored = EditorMap_GetProperty (current, "_color");
	if (authored && !strcmp (authored, canonical) && !EditorMap_GetProperty (current, "color"))
	{ Editor_Status ("Light color unchanged"); return true; }
	editor_sector_map_t sectors; editor_map_t map, *points;
	if (!Editor_ClonePointDocument (&sectors, &map, &points)) return false;
	if (!EditorMap_SetLightColor (points, (size_t)index, rgb))
	{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not set light color"); return false; }
	Editor_CommitPointDocument (&sectors, &map); Editor_Status ("Light color updated"); return true;
}
static qboolean Editor_CreateModel (void *ctx, const char *path, const vec3_t origin)
{
	(void)ctx;
	if (!editor.has_map || !EditorMap_ValidModelPath (path))
	{ Editor_Status ("Model needs a game-relative .mdl, .md3 or .md5mesh path"); return false; }
	double point[3];for(unsigned i=0;i<3;++i)point[i]=origin[i];
	editor_sector_map_t sectors;editor_map_t map,*points;size_t index;
	if (!Editor_ClonePointDocument (&sectors,&map,&points)) return false;
	if (!EditorMap_AddModel (points,path,point,&index) || index>INT_MAX)
	{ Editor_FreePointDocument (&sectors,&map);Editor_Status ("Could not create model instance");return false; }
	Editor_CommitPointDocument (&sectors,&map);editor.selected=-1;editor.selected_point=(int)index;
	Editor_Status ("Static model placed; edit its transform in Properties");return true;
}
static qboolean Editor_PointModel (void *ctx, int index, editor_map_model_t *model)
{ (void)ctx;return index>=0 && EditorMap_GetModel (Editor_PointMap (),(size_t)index,model); }
static qboolean Editor_PointModelBounds (void *ctx,int index,vec3_t corners[8])
{
	editor_map_model_t value;if(!Editor_PointModel(ctx,index,&value))return false;
	for(size_t i=0;i<editor.model_bounds_count;++i)
	{
		const editor_model_bounds_t *bounds=&editor.model_bounds[i];
		if(!bounds->valid || strcmp(bounds->path,value.model))continue;
		double mins[3],maxs[3],result[8][3];
		for(unsigned a=0;a<3;++a){mins[a]=bounds->mins[a];maxs[a]=bounds->maxs[a];}
		if(!EditorMap_ModelCorners(&value,mins,maxs,result))return false;
		for(unsigned v=0;v<8;++v)for(unsigned a=0;a<3;++a)corners[v][a]=(float)result[v][a];
		return true;
	}
	return false;
}
static qboolean Editor_SetPointModel (void *ctx, int index, const editor_map_model_t *model)
{
	(void)ctx;editor_map_model_t old;
	if (!model || !memchr(model->model,0,sizeof(model->model)) || index<0 || !EditorMap_GetModel (Editor_PointMap (),(size_t)index,&old)) return false;
	if (!strcmp (old.model,model->model) && !memcmp(old.origin,model->origin,sizeof(old.origin)) &&
		!memcmp(old.angles,model->angles,sizeof(old.angles)) && old.modelscale==model->modelscale && old.skin==model->skin && old.collision==model->collision) return true;
	editor_sector_map_t sectors;editor_map_t map,*points;
	if (!Editor_ClonePointDocument (&sectors,&map,&points)) return false;
	if (!EditorMap_SetModel (points,(size_t)index,model))
	{ Editor_FreePointDocument (&sectors,&map);Editor_Status ("Model path/transform/scale/skin is invalid");return false; }
	Editor_CommitPointDocument (&sectors,&map);Editor_Status ("Static model properties updated");return true;
}
static qboolean Editor_SetPointOrigin (void *ctx, int index, const vec3_t origin)
{
	(void)ctx; const editor_map_t *current = Editor_PointMap (); double point[3], old[3];
	if (!Editor_IsEditablePoint (current, index) || !EditorMap_GetPointOrigin (current, (size_t)index, old)) return false;
	for (int i = 0; i < 3; ++i) { point[i] = origin[i]; if (!isfinite (point[i]) || fabs (point[i]) > EDITOR_SECTOR_COORD_LIMIT) return false; }
	if (point[0] == old[0] && point[1] == old[1] && point[2] == old[2]) return true;
	editor_sector_map_t sectors; editor_map_t map, *points;
	if (!Editor_ClonePointDocument (&sectors, &map, &points)) return false;
	if (!EditorMap_SetPointOrigin (points, (size_t)index, point))
	{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not move point entity"); return false; }
	Editor_CommitPointDocument (&sectors, &map); Editor_Status ("Point entity moved"); return true;
}
static qboolean Editor_RemovePoint (void *ctx, int index)
{
	(void)ctx; if (!Editor_IsEditablePoint (Editor_PointMap (), index)) return false;
	editor_sector_map_t sectors; editor_map_t map, *points;
	if (!Editor_ClonePointDocument (&sectors, &map, &points)) return false;
	if (!EditorMap_RemoveEntity (points, (size_t)index))
	{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not delete light"); return false; }
	Editor_CommitPointDocument (&sectors, &map); editor.selected_point = -1; Editor_Status ("Light deleted"); return true;
}
static qboolean Editor_DuplicateSelected (void *ctx)
{
	(void)ctx; const double offset[3] = {32, 32, 0}; size_t index;
	if (Editor_IsEditablePoint (Editor_PointMap (), editor.selected_point))
	{
		double origin[3], light_offset[3] = {32, 32, 0};
		if (!EditorMap_GetPointOrigin (Editor_PointMap (), (size_t)editor.selected_point, origin)) return false;
		if (editor.sector_mode && Editor_IsLight (Editor_PointMap (),editor.selected_point))
		{
			static const double candidates[][3] = {{32,32,0},{-32,32,0},{32,-32,0},{-32,-32,0},
				{32,0,0},{-32,0,0},{0,32,0},{0,-32,0},{0,0,32},{0,0,-32},
				{16,16,0},{-16,16,0},{16,-16,0},{-16,-16,0},
				{16,0,0},{-16,0,0},{0,16,0},{0,-16,0},{0,0,16},{0,0,-16}};
			double positions[countof (candidates) + 1][3]; size_t candidate_ids[countof (candidates)], count = 1;
			bool open[countof (candidates) + 1], has_world = false; char error[256] = "";
			memcpy (positions[0], origin, sizeof (origin));
			for (size_t c = 0; c < countof (candidates); ++c)
			{
				double point[3]; qboolean valid = true;
				for (int i = 0; i < 3; ++i) { point[i] = origin[i] + candidates[c][i]; if (!isfinite (point[i]) || fabs (point[i]) > EDITOR_SECTOR_COORD_LIMIT) valid = false; }
				if (valid) { memcpy (positions[count], point, sizeof (point)); candidate_ids[count - 1] = c; ++count; }
			}
			if (!EditorSector_QueryPointsInOpenWorld (&editor.sectors, (const double (*)[3])positions, count, open, &has_world, error, sizeof (error)))
			{ Editor_Status (*error ? error : "Could not check light position"); return false; }
			if (has_world && open[0])
			{
				qboolean found = false;
				for (size_t c = 1; c < count; ++c)
					if (open[c]) { memcpy (light_offset, candidates[candidate_ids[c - 1]], sizeof (light_offset)); found = true; break; }
				if (!found) { Editor_Status ("No open nearby position for the duplicated light"); return false; }
			}
		}
		for (int i = 0; i < 3; ++i)
			if (!isfinite (origin[i] + light_offset[i]) || fabs (origin[i] + light_offset[i]) > EDITOR_SECTOR_COORD_LIMIT)
			{ Editor_Status ("Duplicated light would exceed the editor coordinate range"); return false; }
		editor_sector_map_t sectors; editor_map_t map, *points;
		if (!Editor_ClonePointDocument (&sectors, &map, &points)) return false;
		if (!EditorMap_DuplicatePoint (points, (size_t)editor.selected_point, light_offset, &index) || index > INT_MAX)
		{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not duplicate light"); return false; }
		Editor_CommitPointDocument (&sectors, &map); editor.selected = -1; editor.selected_point = (int)index; Editor_Status ("Entity duplicated"); return true;
	}
	if (!editor.sector_mode || editor.selected < 0 || (size_t)editor.selected >= editor.sectors.num_sectors ||
		editor.sectors.sectors[editor.selected].prop_type == EDITOR_SECTOR_WORLD)
	{ Editor_Status ("Select a light, model or sector prop to duplicate"); return false; }
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_DuplicateProp (&next, (size_t)editor.selected, offset, &index, error, sizeof (error)) || index > INT_MAX)
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not duplicate sector prop"); return false; }
	Editor_CommitSector (&next); editor.selected = (int)index; editor.selected_point = -1; Editor_Status ("Entity duplicated"); return true;
}
static qboolean Editor_SetBounds (void *ctx, int index, const vec3_t mins, const vec3_t maxs)
{
	if (editor.sector_mode)
	{
		vec3_t oldmin, oldmax;
		if (!Editor_Bounds (ctx, index, oldmin, oldmax)) return false;
		double delta[3]; qboolean translate = true;
		for (int i = 0; i < 3; ++i)
		{
			if (!isfinite (mins[i]) || !isfinite (maxs[i]) || maxs[i] <= mins[i] || oldmax[i] <= oldmin[i]) return false;
			delta[i] = mins[i] - oldmin[i];
			if (fabs ((maxs[i] - mins[i]) - (oldmax[i] - oldmin[i])) > 0.01) translate = false;
		}
		editor_sector_map_t next; char error[256] = "";
		if (!Editor_CloneSector (&next)) return false;
		if (translate)
		{
			if (!EditorSector_Translate (&next, (size_t)index, delta, error, sizeof (error)))
			{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not move sector"); return false; }
		}
		else
		{
			bool *seen = calloc (next.num_vertices, sizeof (*seen));
			if (!seen) { EditorSector_Free (&next); Editor_Status ("Out of memory resizing sector"); return false; }
			const editor_sector_t *sector = &next.sectors[index];
			for (size_t f = 0; f < sector->num_faces; ++f)
				for (size_t v = 0; v < sector->faces[f].num_vertices; ++v)
				{
					size_t vertex = sector->faces[f].vertices[v];
					if (vertex >= next.num_vertices || seen[vertex]) continue;
					seen[vertex] = true;
					for (int axis = 0; axis < 3; ++axis)
						next.vertices[vertex].position[axis] = mins[axis] + (next.vertices[vertex].position[axis] - oldmin[axis]) *
							(maxs[axis] - mins[axis]) / (oldmax[axis] - oldmin[axis]);
				}
			free (seen);
			if (!EditorSector_Validate (&next, error, sizeof (error)))
			{ EditorSector_Free (&next); Editor_Status (*error ? error : "Sector resize would make invalid geometry"); return false; }
		}
		Editor_CommitSector (&next); Editor_Status (translate ? "Sector moved" : "Sector resized"); return true;
	}
	(void)ctx; editor_map_t next;
	if (!Editor_Clone (&next)) return false;
	editor_map_brush_t *brush = Editor_Brush (&next, index, NULL, NULL);
	double oldmin[3], oldmax[3], a[3], b[3], delta[3]; qboolean translate = true;
	if (!brush || !EditorMap_BrushBounds (brush, oldmin, oldmax)) { EditorMap_Free (&next); return false; }
	for (int i = 0; i < 3; ++i)
	{
		a[i] = mins[i]; b[i] = maxs[i]; delta[i] = a[i] - oldmin[i];
		if (fabs ((b[i] - a[i]) - (oldmax[i] - oldmin[i])) > 0.01) translate = false;
	}
	if (!(translate ? EditorMap_BrushTranslate (brush, delta) : EditorMap_BrushResizeBox (brush, a, b)))
	{
		EditorMap_Free (&next); Editor_Status ("Only axis-aligned boxes can be resized"); return false;
	}
	Editor_Commit (&next); Editor_Status (translate ? "Brush moved" : "Brush resized"); return true;
}
static void Editor_RemoveSelected (void *ctx)
{
	if (editor.sector_mode)
	{
		if (editor.selected < 0) return;
		editor_sector_map_t next; char error[256] = "";
		if (!Editor_CloneSector (&next)) return;
		if (!EditorSector_Remove (&next, (size_t)editor.selected, error, sizeof (error)))
		{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not delete sector"); return; }
		Editor_CommitSector (&next); editor.selected = -1; Editor_Status ("Sector deleted"); return;
	}
	(void)ctx; editor_map_t next; size_t e, b;
	if (editor.selected < 0 || !Editor_Clone (&next)) return;
	if (!Editor_Brush (&next, editor.selected, &e, &b) || !EditorMap_RemoveBrush (&next.entities[e], b)) { EditorMap_Free (&next); return; }
	Editor_Commit (&next); editor.selected = -1; Editor_Status ("Brush deleted");
}
static int Editor_FindSpawn (const editor_map_t *map)
{
	for (size_t i = 1; i < map->num_entities; ++i)
	{
		const char *classname = EditorMap_GetProperty (&map->entities[i], "classname");
		if (classname && !strcmp (classname, "info_player_start")) return (int)i;
	}
	return -1;
}
static void Editor_PlaceSpawn (void *ctx, const vec3_t origin)
{
	if (editor.sector_mode)
	{
		editor_sector_map_t next; size_t entity;
		if (!Editor_CloneSector (&next)) return;
		int found = Editor_FindSpawn (&next.entities);
		if (found < 0) { if (!EditorMap_AddEntity (&next.entities, "info_player_start", &entity)) { EditorSector_Free (&next); return; } }
		else entity = (size_t)found;
		char value[96]; q_snprintf (value, sizeof (value), "%.0f %.0f %.0f", origin[0], origin[1], origin[2]);
		if (!EditorMap_SetProperty (&next.entities.entities[entity], "origin", value)) { EditorSector_Free (&next); return; }
		Editor_CommitSector (&next); Editor_Status ("Player spawn placed"); return;
	}
	(void)ctx; editor_map_t next; size_t entity;
	if (!Editor_Clone (&next)) return;
	int found = Editor_FindSpawn (&next);
	if (found < 0) { if (!EditorMap_AddEntity (&next, "info_player_start", &entity)) { EditorMap_Free (&next); return; } }
	else entity = (size_t)found;
	char value[96]; q_snprintf (value, sizeof (value), "%.0f %.0f %.0f", origin[0], origin[1], origin[2]);
	if (!EditorMap_SetProperty (&next.entities[entity], "origin", value)) { EditorMap_Free (&next); return; }
	Editor_Commit (&next); Editor_Status ("Player spawn placed");
}
static qboolean Editor_GetSpawn (void *ctx, vec3_t origin)
{
	(void)ctx; const editor_map_t *map = editor.sector_mode ? &editor.sectors.entities : &editor.map;
	int index = Editor_FindSpawn (map);
	if (index < 0) return false;
	const char *s = EditorMap_GetProperty (&map->entities[index], "origin");
	return s && sscanf (s, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3 && isfinite (origin[0]) && isfinite (origin[1]) && isfinite (origin[2]);
}

static qboolean Editor_IsSectorMode (void *ctx) { (void)ctx; return editor.sector_mode; }
static uint64_t Editor_SectorRevision (void *ctx) { (void)ctx; return editor.revision; }
static int Editor_SectorPropType (void *ctx, int sector)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors) return EDITOR_SECTOR_WORLD;
	return (int)editor.sectors.sectors[sector].prop_type;
}
static qboolean Editor_SectorSetPropType (void *ctx, int sector, int type)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors || type < EDITOR_SECTOR_WORLD || type > EDITOR_SECTOR_PROP_MOVABLE) return false;
	if ((int)editor.sectors.sectors[sector].prop_type == type) return true;
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_SetPropType (&next, (size_t)sector, (editor_sector_prop_type_t)type, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not change sector type"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector type changed"); return true;
}
static qboolean Editor_SectorCollisionEnabled (void *ctx, int sector)
{
	(void)ctx;
	return editor.sector_mode && sector >= 0 && (size_t)sector < editor.sectors.num_sectors &&
		editor.sectors.sectors[sector].prop_type == EDITOR_SECTOR_PROP_STATIC && !editor.sectors.sectors[sector].prop_no_collision;
}
static qboolean Editor_SectorSetCollisionEnabled (void *ctx, int sector, qboolean enabled)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors ||
		editor.sectors.sectors[sector].prop_type != EDITOR_SECTOR_PROP_STATIC) return false;
	if ((!editor.sectors.sectors[sector].prop_no_collision) == !!enabled) return true;
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_SetPropCollisionEnabled (&next, (size_t)sector, enabled, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not change prop collision"); return false; }
	Editor_CommitSector (&next); Editor_Status (enabled ? "Prop collision enabled" : "Prop collision disabled"); return true;
}
static int Editor_SectorFaceId (void *ctx, int sector, int polygon)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || sector >= editor.geometry_count || polygon < 0 || polygon >= editor.geometry[sector].count) return -1;
	return editor.geometry[sector].faces[polygon].source_face;
}
static int Editor_SectorSourceFaceVertices (void *ctx, int sector, int face, vec3_t *vertices, int capacity)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors || face < 0 ||
		(size_t)face >= editor.sectors.sectors[sector].num_faces) return 0;
	const editor_sector_face_t *source = &editor.sectors.sectors[sector].faces[face];
	if (vertices && capacity > 0)
		for (int i = 0; i < capacity && (size_t)i < source->num_vertices; ++i)
			for (int axis = 0; axis < 3; ++axis)
				vertices[i][axis] = (float)editor.sectors.vertices[source->vertices[i]].position[axis];
	return (int)source->num_vertices;
}
static qboolean Editor_SectorFaceIsPortal (void *ctx, int sector, int face)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors || face < 0 ||
		(size_t)face >= editor.sectors.sectors[sector].num_faces) return false;
	return editor.sectors.sectors[sector].faces[face].portal_sector != EDITOR_SECTOR_NO_PORTAL;
}
static int Editor_SectorFacePortalPeer (void *ctx, int sector, int face)
{
	(void)ctx;
	if (!Editor_SectorFaceIsPortal (NULL, sector, face)) return -1;
	size_t peer = editor.sectors.sectors[sector].faces[face].portal_sector;
	return peer <= INT_MAX ? (int)peer : -1;
}
static qboolean Editor_SectorMoveFace (void *ctx, int sector, int face, const vec3_t delta)
{
	(void)ctx; if (!editor.sector_mode || sector < 0 || face < 0) return false;
	editor_sector_map_t next; char error[256] = ""; double movement[3] = {delta[0], delta[1], delta[2]};
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_MoveFace (&next, (size_t)sector, (size_t)face, movement, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not move sector face"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector face moved"); return true;
}
/* A stable per-sector vertex order is derived from first occurrence in its
 * faces; the document itself stores shared global vertex indices. */
static int Editor_SectorVertexIndex (int sector, int local, size_t *out)
{
	if (!editor.sector_mode || sector < 0 || sector >= editor.geometry_count || local < -1) return 0;
	const editor_brush_geometry_t *geometry = &editor.geometry[sector];
	if (local >= 0 && local < geometry->vertex_count && out) *out = geometry->vertex_indices[local];
	return geometry->vertex_count;
}
static int Editor_SectorVertexCount (void *ctx, int sector) { (void)ctx; return Editor_SectorVertexIndex (sector, -1, NULL); }
static qboolean Editor_SectorVertexSameAxis (void *ctx, int sector, int vertex, int other_sector, int other_vertex, int axis)
{
	(void)ctx; size_t first = 0, second = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &first);
	int other_count = Editor_SectorVertexIndex (other_sector, other_vertex, &second);
	if (axis < 0 || axis >= 3 || vertex < 0 || vertex >= count || other_vertex < 0 || other_vertex >= other_count ||
		first >= editor.sectors.num_vertices || second >= editor.sectors.num_vertices) return false;
	return editor.sectors.vertices[first].position[axis] == editor.sectors.vertices[second].position[axis];
}
static qboolean Editor_SectorVerticesCoincide (int sector_a, size_t a, int sector_b, size_t b)
{
	if (sector_a < 0 || sector_b < 0) return false;
	return EditorSector_VerticesEditTogether (&editor.sectors, (size_t)sector_a, a, (size_t)sector_b, b);
}
static qboolean Editor_SectorVertexCoincident (void *ctx, int sector, int vertex, int other_sector, int other_vertex)
{
	(void)ctx; size_t a = 0, b = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &a);
	int other_count = Editor_SectorVertexIndex (other_sector, other_vertex, &b);
	return vertex >= 0 && vertex < count && other_vertex >= 0 && other_vertex < other_count && Editor_SectorVerticesCoincide (sector, a, other_sector, b);
}
static qboolean Editor_SectorFaceVertexCoincident (void *ctx, int sector, int face, int corner,
	int source_sector, int source_vertex, qboolean display_polygon)
{
	(void)ctx; size_t source = 0, target;
	int count = Editor_SectorVertexIndex (source_sector, source_vertex, &source);
	if (source_vertex < 0 || source_vertex >= count || sector < 0 || (size_t)sector >= editor.sectors.num_sectors || face < 0 || corner < 0) return false;
	if (display_polygon)
	{
		if (sector >= editor.geometry_count || face >= editor.geometry[sector].count) return false;
		const editor_face_geometry_t *geometry = &editor.geometry[sector].faces[face];
		if (corner >= geometry->count || !geometry->vertex_indices) return false;
		target = geometry->vertex_indices[corner];
	}
	else
	{
		const editor_sector_t *s = &editor.sectors.sectors[sector];
		if ((size_t)face >= s->num_faces || (size_t)corner >= s->faces[face].num_vertices) return false;
		target = s->faces[face].vertices[corner];
	}
	return Editor_SectorVerticesCoincide (source_sector, source, sector, target);
}
static qboolean Editor_SectorVertex (void *ctx, int sector, int vertex, vec3_t position)
{
	(void)ctx; size_t global = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &global);
	if (vertex < 0 || vertex >= count) return false;
	for (int i = 0; i < 3; ++i) position[i] = (float)editor.sectors.vertices[global].position[i];
	return true;
}
static qboolean Editor_SectorSameVertex (void *ctx, int sector, int vertex, int other_sector, int other_vertex)
{
	(void)ctx; size_t first = 0, second = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &first);
	int other_count = Editor_SectorVertexIndex (other_sector, other_vertex, &second);
	return vertex >= 0 && vertex < count && other_vertex >= 0 && other_vertex < other_count && first == second;
}
static qboolean Editor_SectorSetVertex (void *ctx, int sector, int vertex, const vec3_t position)
{
	(void)ctx; size_t global = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &global);
	if (vertex < 0 || vertex >= count) return false;
	editor_sector_map_t next; char error[256] = ""; double point[3] = {position[0], position[1], position[2]};
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_SetVertex (&next, global, point, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not move sector vertex"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector vertex moved"); return true;
}
static qboolean Editor_SectorMergeVertex (void *ctx, int sector, int vertex, int target_sector, int target_vertex)
{
	(void)ctx; size_t source = 0, target = 0;
	int count = Editor_SectorVertexIndex (sector, vertex, &source);
	int target_count = Editor_SectorVertexIndex (target_sector, target_vertex, &target);
	if (vertex < 0 || vertex >= count || target_vertex < 0 || target_vertex >= target_count) return false;
	if (source == target) return true;
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_MergeVertices (&next, source, target, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not merge sector vertices"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector vertices merged"); return true;
}
static qboolean Editor_SectorKnife (void *ctx, int sector, int face, const vec3_t start, const vec3_t end)
{
	(void)ctx; if (!editor.sector_mode || sector < 0 || face < 0) return false;
	editor_sector_map_t next; char error[256] = "";
	double a[3] = {start[0], start[1], start[2]}, b[3] = {end[0], end[1], end[2]};
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_CutFace (&next, (size_t)sector, (size_t)face, a, b, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not cut sector face"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector face cut"); return true;
}
static qboolean Editor_SectorLink (void *ctx, int sector, int face, int other_sector, int other_face)
{
	(void)ctx; if (!editor.sector_mode || sector < 0 || face < 0 || other_sector < 0 || other_face < 0) return false;
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_LinkFaceRegions (&next, (size_t)sector, (size_t)face, (size_t)other_sector, (size_t)other_face, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not link sector portal"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector portal linked"); return true;
}
static qboolean Editor_SectorUnlink (void *ctx, int sector, int face)
{
	(void)ctx; if (!editor.sector_mode || sector < 0 || face < 0) return false;
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return false;
	if (!EditorSector_UnlinkPortalGroup (&next, (size_t)sector, (size_t)face, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not unlink sector portal"); return false; }
	Editor_CommitSector (&next); Editor_Status ("Sector portal unlinked"); return true;
}
static qboolean Editor_SectorApplyMaterial (void *ctx, int sector, int face)
{
	(void)ctx;
	if (!editor.sector_mode || sector < 0 || (size_t)sector >= editor.sectors.num_sectors || face < 0 ||
		(size_t)face >= editor.sectors.sectors[sector].num_faces || !*editor_texture.string ||
		strlen (editor_texture.string) >= EDITOR_SECTOR_MATERIAL_MAX) return false;
	for (const unsigned char *p = (const unsigned char *)editor_texture.string; *p; ++p)
		if (*p < 33 || *p > 126 || isspace (*p) || *p == '"' || *p == '{' || *p == '}' || *p == '(' || *p == ')')
		{ Editor_Status ("Invalid editor_texture material name"); return false; }
	editor_sector_map_t next;
	if (!Editor_CloneSector (&next)) return false;
	q_strlcpy (next.sectors[sector].faces[face].material, editor_texture.string, EDITOR_SECTOR_MATERIAL_MAX);
	Editor_CommitSector (&next); Editor_Status ("Sector material changed"); return true;
}

static qboolean Editor_WriteChecked (const char *path, const char *data, size_t length, qboolean exclusive)
{
	FILE *file = Sys_fopen (path, exclusive ? "wbx" : "wb");
	if (!file) return false;
	qboolean ok = fwrite (data, 1, length, file) == length && fflush (file) == 0;
	if (fclose (file) != 0) ok = false;
	if (!ok && exclusive) Sys_remove (path);
	return ok;
}
static qboolean Editor_Save (const char *name)
{
	if (!Editor_ValidName (name)) { Editor_Status ("Choose a map name using letters, numbers, _ or -"); return false; }
	char path[MAX_OSPATH], temp[MAX_OSPATH], dir[MAX_OSPATH];
	if (!Editor_MapPath (name, "map", path) || q_snprintf (dir, sizeof (dir), "%s/maps", com_gamedir) >= (int)sizeof (dir))
	{ Editor_Status ("Map path is too long"); return false; }
	Sys_mkdir (dir);
	char *data = NULL; size_t size = 0;
	if (!(editor.sector_mode ? EditorSector_Serialize (&editor.sectors, &data, &size) : EditorMap_Serialize (&editor.map, &data, &size)))
	{ Editor_Status ("Could not serialize map"); return false; }
	qboolean same = *editor.name && !strcmp (editor.name, name), ok = false;
	if (!same)
		ok = Editor_WriteChecked (path, data, size, true); /* save-as never overwrites */
	else
	{
		for (unsigned i = 0; i < 100; ++i)
		{
			int n = q_snprintf (temp, sizeof (temp), "%s.quiver-tmp-%u", path, i);
			if (n < 0 || n >= (int)sizeof (temp)) break;
			if (Sys_FileType (temp) != FS_ENT_NONE) continue;
			if (!Editor_WriteChecked (temp, data, size, true)) break;
			ok = Sys_rename (temp, path) == 0;
			if (!ok) Sys_remove (temp);
			break;
		}
	}
	free (data);
	if (!ok) { Editor_Status ("Save failed: destination exists or cannot be written; original preserved"); return false; }
	q_strlcpy (editor.name, name, sizeof (editor.name)); editor.dirty = false; Editor_Status ("Map saved"); return true;
}
static void Editor_Enter (void)
{
	editor.playtesting = false;
	Key_ClearStates ();
	key_dest = key_game;
	key_lines[edit_line][1] = 0;
	key_linepos = 1;
	Con_ClearNotify ();
	IN_Deactivate (true);
	EditorUI_SetActive (true);
}
static void Editor_ResetDocument (editor_map_t *next, const char *name, qboolean dirty)
{
	EditorMap_Free (&editor.map); Editor_FreeHistory (editor.undo, &editor.nundo); Editor_FreeHistory (editor.redo, &editor.nredo);
	EditorSector_Free (&editor.sectors); Editor_FreeSectorHistory (editor.sector_undo, &editor.nsector_undo); Editor_FreeSectorHistory (editor.sector_redo, &editor.nsector_redo);
	editor.sector_mode = false;
	editor.map = *next; memset (next, 0, sizeof (*next));
	editor.selected = editor.selected_point = -1; editor.has_map = true; editor.dirty = dirty; ++editor.revision;
	Editor_RebuildGeometry ();
	vec3_t focus;
	if (!Editor_GetSpawn (NULL, focus))
	{
		vec3_t mins, maxs;
		if (Editor_Bounds (NULL, 0, mins, maxs))
			for (int i = 0; i < 3; ++i) focus[i] = (mins[i] + maxs[i]) * 0.5f;
		else focus[0] = focus[1] = focus[2] = 0;
	}
	EditorUI_Focus (focus);
	q_strlcpy (editor.name, name ? name : "", sizeof (editor.name));
	Editor_Enter ();
}
static void Editor_ResetSectorDocument (editor_sector_map_t *next, const char *name, qboolean dirty)
{
	EditorMap_Free (&editor.map); Editor_FreeHistory (editor.undo, &editor.nundo); Editor_FreeHistory (editor.redo, &editor.nredo);
	EditorSector_Free (&editor.sectors); Editor_FreeSectorHistory (editor.sector_undo, &editor.nsector_undo); Editor_FreeSectorHistory (editor.sector_redo, &editor.nsector_redo);
	editor.sector_mode = true;
	editor.sectors = *next; memset (next, 0, sizeof (*next));
	editor.selected = editor.selected_point = -1; editor.has_map = true; editor.dirty = dirty; ++editor.revision;
	Editor_RebuildGeometry ();
	vec3_t focus;
	if (!Editor_GetSpawn (NULL, focus))
	{
		vec3_t mins, maxs;
		if (Editor_Bounds (NULL, 0, mins, maxs))
			for (int i = 0; i < 3; ++i) focus[i] = (mins[i] + maxs[i]) * 0.5f;
		else focus[0] = focus[1] = focus[2] = 0;
	}
	EditorUI_Focus (focus);
	q_strlcpy (editor.name, name ? name : "", sizeof (editor.name));
	Editor_Enter ();
}
static qboolean Editor_NewBrushDocument (void)
{
	editor_map_t next = {0};
	const double boxes[6][2][3] = {
		{{-288,-288,-32},{288,288,0}}, {{-288,-288,256},{288,288,288}},
		{{-288,-288,0},{-256,288,256}}, {{256,-288,0},{288,288,256}},
		{{-256,-288,0},{256,-256,256}}, {{-256,256,0},{256,288,256}}};
	if (!EditorMap_Init (&next)) return false;
	for (int i = 0; i < 6; ++i)
		if (!EditorMap_AddBox (&next, 0, boxes[i][0], boxes[i][1], editor_texture.string, NULL)) { EditorMap_Free (&next); return false; }
	size_t entity;
	if (!EditorMap_AddEntity (&next, "info_player_start", &entity) || !EditorMap_SetProperty (&next.entities[entity], "origin", "0 0 32"))
	{ EditorMap_Free (&next); return false; }
	if (!EditorMap_AddEntity (&next, "light", &entity) || !EditorMap_SetProperty (&next.entities[entity], "origin", "0 0 192") ||
		!EditorMap_SetProperty (&next.entities[entity], "light", "400"))
	{ EditorMap_Free (&next); return false; }
	Editor_ResetDocument (&next, "", true); Editor_Status ("New sealed room; use editor_save <name> to save"); return true;
}
static qboolean Editor_NewDocument (void)
{
	editor_sector_map_t next = {0};
	const double mins[3] = {-256, -256, 0}, maxs[3] = {256, 256, 256};
	char error[256] = ""; size_t entity;
	if (!EditorSector_Init (&next) || !EditorSector_AddBox (&next, mins, maxs, editor_texture.string, NULL, error, sizeof (error)) ||
		!EditorMap_AddEntity (&next.entities, "info_player_start", &entity) || !EditorMap_SetProperty (&next.entities.entities[entity], "origin", "0 0 32") ||
		!EditorMap_AddEntity (&next.entities, "light", &entity) || !EditorMap_SetProperty (&next.entities.entities[entity], "origin", "0 0 192") ||
		!EditorMap_SetProperty (&next.entities.entities[entity], "light", "400"))
	{
		EditorSector_Free (&next); if (*error) Editor_Status (error); return false;
	}
	Editor_ResetSectorDocument (&next, "", true); Editor_Status ("New 3D sector map; use editor_save <name> to save"); return true;
}
static qboolean Editor_Open (const char *name)
{
	if (!Editor_ValidName (name)) { Editor_Status ("Map name may contain only letters, numbers, _ and -"); return false; }
	char path[MAX_OSPATH], error[256] = "";
	if (!Editor_MapPath (name, "map", path)) { Editor_Status ("Map path is too long"); return false; }
	FILE *file = Sys_fopen (path, "rb");
	if (!file) { Editor_Status ("Could not open source map"); return false; }
	if (fseek (file, 0, SEEK_END) || ftell (file) < 0) { fclose (file); Editor_Status ("Could not read source map"); return false; }
	long length = ftell (file);
	if ((unsigned long)length > EDITOR_MAX_SOURCE || fseek (file, 0, SEEK_SET)) { fclose (file); Editor_Status ("Source map is too large"); return false; }
	char *data = malloc ((size_t)length + 1);
	if (!data) { fclose (file); Editor_Status ("Out of memory reading map"); return false; }
	qboolean ok = fread (data, 1, (size_t)length, file) == (size_t)length;
	fclose (file); data[length] = 0;
	if (ok && EditorSector_IsFormat (data, (size_t)length))
	{
		editor_sector_map_t next = {0};
		ok = EditorSector_Parse (&next, data, (size_t)length, error, sizeof (error));
		free (data);
		if (!ok) { EditorSector_Free (&next); Editor_Status ("Could not parse sector map; see console"); if (*error) Con_Printf ("%s\n", error); return false; }
		Editor_ResetSectorDocument (&next, name, false); Editor_Status ("Sector map opened"); return true;
	}
	editor_map_t next = {0};
	if (ok) ok = EditorMap_Parse (&next, data, (size_t)length, error, sizeof (error));
	free (data);
	if (!ok) { EditorMap_Free (&next); Editor_Status ("Could not parse source map; see console"); if (*error) Con_Printf ("%s\n", error); return false; }
	Editor_ResetDocument (&next, name, false); Editor_Status ("Source map opened"); return true;
}
static qboolean Editor_PlaytestOutputExists (void)
{
	FILE *file = Sys_fopen (editor.bsp_path, "rb");
	if (!file) return false;
	qboolean ok = fseek (file, 0, SEEK_END) == 0 && ftell (file) >= 124;
	fclose (file); return ok;
}
static qboolean Editor_CopyFile (const char *source, const char *destination)
{
	FILE *in = Sys_fopen (source, "rb"), *out;
	if (!in) return false;
	out = Sys_fopen (destination, "wb");
	if (!out) { fclose (in); return false; }
	char data[16384]; size_t n; qboolean ok = true;
	while ((n = fread (data, 1, sizeof (data), in)) != 0)
		if (fwrite (data, 1, n, out) != n) { ok = false; break; }
	if (ferror (in)) ok = false;
	if (fclose (out)) ok = false;
	fclose (in);
	if (!ok) Sys_remove (destination);
	return ok;
}
static qboolean Editor_StartScriptBuild (void)
{
	char base[MAX_OSPATH], tool[MAX_OSPATH], qcc[MAX_OSPATH], qcsource[MAX_OSPATH], source[MAX_OSPATH], error[256] = "";
	if (!EditorTools_GetSelfPath (host_parms->argv[0], base, sizeof (base))) return false;
	char *slash = strrchr (base, '/');
#ifdef _WIN32
	char *backslash = strrchr (base, '\\'); if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
	if (!slash) { Editor_Status ("Cannot locate script tools beside the executable"); return false; }
	slash[1] = 0;
#ifdef _WIN32
	const char *suffix = ".exe";
#else
	const char *suffix = "";
#endif
	if (q_snprintf (tool, sizeof (tool), "%squiverscript%s", base, suffix) >= (int)sizeof (tool) ||
		q_snprintf (qcc, sizeof (qcc), "%sfteqcc%s", base, suffix) >= (int)sizeof (qcc) ||
		q_snprintf (qcsource, sizeof (qcsource), "%sid1-quakec", base) >= (int)sizeof (qcsource) ||
		q_snprintf (source, sizeof (source), "%s/%s", com_gamedir, editor.script_path) >= (int)sizeof (source) ||
		q_snprintf (editor.script_package, sizeof (editor.script_package), "%s/%s", COM_GetWriteRoot (), editor.build_name) >= (int)sizeof (editor.script_package) ||
		q_snprintf (editor.script_snapshot, sizeof (editor.script_snapshot), "%s/project.qs", editor.script_package) >= (int)sizeof (editor.script_snapshot))
	{ Editor_Status ("Script build path is too long"); return false; }
	if (Sys_FileType (tool) != FS_ENT_FILE || Sys_FileType (qcc) != FS_ENT_FILE || Sys_FileType (qcsource) != FS_ENT_DIRECTORY)
	{ Editor_Status ("QuiverScript tools are missing beside the executable"); return false; }
	if (Sys_FileType (editor.script_package) != FS_ENT_NONE)
	{ Editor_Status ("Script playtest directory already exists"); return false; }
	Sys_mkdir (editor.script_package);
	if (!Editor_CopyFile (source, editor.script_snapshot))
	{ Editor_Status ("Could not read world script in the current game directory"); return false; }
	const char *args[] = {tool, "build", "--script", editor.script_snapshot, "--game-dir", editor.script_package,
		"--qcc", qcc, "--qc-source", qcsource, NULL};
	editor.process = Editor_ProcessStart (args, error, sizeof (error));
	if (!editor.process)
	{ Editor_Status ("Could not start QuiverScript compiler"); if (*error) Con_Printf ("%s\n", error); return false; }
	editor.build_stage = EDITOR_BUILD_SCRIPT;
	Editor_Status ("Compiling world script...");
	return true;
}
/* The canonical game list uses semicolons as separators, not shell syntax.
 * Validate bounds before publishing any pointers into the caller's storage. */
static const char *Editor_AppendGameDirectories (const char *names, char *storage, size_t storage_size,
	const char **args, size_t capacity, size_t *argc)
{
	if (!names || !storage || !storage_size || !args || !argc || *argc>capacity) return "Invalid playtest game directory arguments";
	size_t length=strlen(names),games=length?1:0;
	if(length>=storage_size)return "Original game directory list is too long for scripted playtest";
	for(size_t i=0;i<length;++i)
		if(names[i]==';')
		{
			if(!i || i+1==length || names[i-1]==';')return "Original game directory list contains an empty name";
			++games;
		}
	if(games>(capacity-*argc)/2)return "Too many original game directories for scripted playtest";
	memcpy(storage,names,length+1);
	char *name=storage;
	for(size_t i=0;i<games;++i)
	{
		char *separator=strchr(name,';');if(separator)*separator=0;
		args[(*argc)++]="-game";args[(*argc)++]=name;
		if(separator)name=separator+1;
	}
	return NULL;
}
static qboolean Editor_LaunchScriptPlaytest (void)
{
	char self[MAX_OSPATH], maps[MAX_OSPATH], destination[MAX_OSPATH], error[256] = "";
	if (!EditorTools_GetSelfPath (host_parms->argv[0], self, sizeof (self)) ||
		q_snprintf (maps, sizeof (maps), "%s/maps", editor.script_package) >= (int)sizeof (maps) ||
		q_snprintf (destination, sizeof (destination), "%s/%s.bsp", maps, editor.build_name) >= (int)sizeof (destination))
	{ Editor_Status ("Script playtest path is too long"); return false; }
	Sys_mkdir (maps);
	if (!Editor_CopyFile (editor.bsp_path, destination))
	{ Editor_Status ("Could not copy the map into the script package"); return false; }
	const char *args[MAX_NUM_ARGVS + 1]; size_t argc = 0; char original_games[1024];
	args[argc++] = self; args[argc++] = "-basedir"; args[argc++] = com_basedir;
	args[argc++] = "-userdir"; args[argc++] = host_parms->userdir;
	/* Reserve final package, playtest controls, optional flags and terminator.
	 * Earlier mods retain their normal search order; generated progs.dat wins. */
	const char *game_error=Editor_AppendGameDirectories(COM_GetGameNames(false),original_games,sizeof(original_games),
		args,countof(args)-9,&argc);
	if(game_error){Editor_Status(game_error);return false;}
	args[argc++] = "-game"; args[argc++] = editor.build_name;
	args[argc++] = "-quiver-editor-playtest"; args[argc++] = editor.build_name;
	args[argc++] = "-quiver-playtest-edicts"; args[argc++] = max_edicts.string;
	if (COM_CheckParm ("-nosound")) args[argc++] = "-nosound";
	if (COM_CheckParm ("-nojoy")) args[argc++] = "-nojoy";
	args[argc] = NULL;
	editor.play_process = Editor_ProcessStart (args, error, sizeof (error));
	if (!editor.play_process)
	{ Editor_Status ("Could not launch scripted playtest"); if (*error) Con_Printf ("%s\n", error); return false; }
	Editor_Status ("Scripted playtest started; press F9 in its window to return");
	return true;
}
static void Editor_LaunchPlaytest (void)
{
	if (!Editor_PlaytestOutputExists ()) { Editor_Status ("Compiled BSP is missing or incomplete"); return; }
	if (*editor.script_path) { Editor_LaunchScriptPlaytest (); return; }
	char command[128]; q_snprintf (command, sizeof (command), "map %s\n", editor.build_name);
	editor.playtesting = true;
	EditorUI_SetActive (false); Cbuf_AddText (command); Editor_Status ("Starting playtest; press F9 to resume editing");
}
static qboolean Editor_CreateBuildPaths (const char *data, size_t length)
{
	char dir[MAX_OSPATH];
	if (q_snprintf (dir, sizeof (dir), "%s/maps", com_gamedir) >= (int)sizeof (dir)) return false;
	Sys_mkdir (dir);
	for (unsigned attempt = 0; attempt < 1000; ++attempt)
	{
		q_snprintf (editor.build_name, sizeof (editor.build_name), "__quiver_editor_%u_%u", (unsigned)time (NULL), ++editor.build_serial);
		if (!Editor_MapPath (editor.build_name, "map", editor.source_path) || !Editor_MapPath (editor.build_name, "bsp", editor.bsp_path)) return false;
		if (Sys_FileType (editor.bsp_path) != FS_ENT_NONE) continue;
		if (!Editor_WriteChecked (editor.source_path, data, length, true)) continue;
		/* Reserve the output as well so no existing BSP can be replaced. */
		if (!Editor_WriteChecked (editor.bsp_path, "", 0, true)) { Sys_remove (editor.source_path); continue; }
		return true;
	}
	return false;
}
/* Only the compiler snapshot receives this fallback; the user's source is unchanged. */
static qboolean Editor_ApplyDefaultWad (editor_map_t *map)
{
	if (!map->num_entities) return false;
	const char *wad = EditorMap_GetProperty (&map->entities[0], "wad");
	const char *alt = EditorMap_GetProperty (&map->entities[0], "_wad");
	if ((wad && *wad) || (alt && *alt)) return true;
#ifdef USE_SDL3
	const char *base = SDL_GetBasePath ();
#else
	char *base = SDL_GetBasePath ();
#endif
	char path[MAX_OSPATH];
	qboolean found = base && q_snprintf (path, sizeof (path), "%seditor_defaults.wad", base) < (int)sizeof (path) && Sys_FileType (path) == FS_ENT_FILE;
#ifndef USE_SDL3
	SDL_free (base);
#endif
	if (!found) return true; /* A custom compiler may have its own texture setup. */
	for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
	return EditorMap_SetProperty (&map->entities[0], "wad", path);
}
static qboolean Editor_StartStage (editor_build_stage_t stage)
{
	char self[MAX_OSPATH], error[256] = "";
	if (!EditorTools_Available () || !EditorTools_GetSelfPath (host_parms->argv[0], self, sizeof (self)))
	{
		Editor_Status ("Embedded map tools are unavailable in this build");
		return false;
	}
	const char *qbsp[] = {self, EDITOR_TOOLS_WORKER_OPTION, "qbsp", "-nopercent", "-leaktest", editor.source_path, editor.bsp_path, NULL};
	const char *qbsp_sector[] = {self, EDITOR_TOOLS_WORKER_OPTION, "qbsp", "-nopercent", "-leaktest", "-bsp2", editor.source_path, editor.bsp_path, NULL};
	const char *custom[] = {editor_qbsp.string, editor.source_path, editor.bsp_path, NULL};
	const char *custom_sector[] = {editor_qbsp.string, "-bsp2", editor.source_path, editor.bsp_path, NULL};
	const char *vis[] = {self, EDITOR_TOOLS_WORKER_OPTION, "vis", "-threads", "4", editor.bsp_path, NULL};
	const char *light[] = {self, EDITOR_TOOLS_WORKER_OPTION, "light", "-threads", "4", editor.bsp_path, NULL};
	const char *const *args = stage == EDITOR_BUILD_QBSP ? (*editor_qbsp.string ? (editor.sector_mode ? custom_sector : custom) :
		(editor.sector_mode ? qbsp_sector : qbsp)) : stage == EDITOR_BUILD_VIS ? vis : light;
	editor.process = Editor_ProcessStart (args, error, sizeof (error));
	if (!editor.process)
	{
		Editor_Status (stage == EDITOR_BUILD_QBSP ? "Could not start QBSP" : stage == EDITOR_BUILD_VIS ? "Could not start VIS" : "Could not start LIGHT");
		if (*error) Con_Printf ("%s\n", error);
		return false;
	}
	editor.build_stage = stage;
	if (stage == EDITOR_BUILD_VIS) Editor_Status ("Calculating visibility...");
	else if (stage == EDITOR_BUILD_LIGHT) Editor_Status ("Calculating lighting...");
	return true;
}
static void Editor_Play (void)
{
	if (editor.process) { Editor_Status ("A map compile is already running"); return; }
	if (editor.play_process) { Editor_Status ("A scripted playtest is already running"); return; }
	const char *script = Editor_WorldScript (NULL);
	if (!Editor_ValidScriptPath (script)) { Editor_Status ("World script must be a project-relative .qs path"); return; }
	q_strlcpy (editor.script_path, script, sizeof (editor.script_path));
	/* A script is an external file, so re-read and compile it for each playtest. */
	const editor_map_t *points=Editor_PointMap();qboolean has_models=false;
	if(points)for(size_t i=1;i<points->num_entities;++i)if(EditorMap_IsModel(points,i)){has_models=true;break;}
	/* Model assets also carry compiled collision data: a model reload must
	 * not launch a BSP holding geometry from a previous asset revision. */
	if (!*editor.script_path && !has_models && editor.compiled_revision == editor.revision && Editor_PlaytestOutputExists ()) { Editor_LaunchPlaytest (); return; }
	Editor_RebuildModelBounds ();
	char *data = NULL; size_t length = 0;
	editor_map_t build = {0};
	char error[256] = "";
	free (editor.static_model_lump); editor.static_model_lump=NULL; editor.static_model_lump_bytes=0;
	qboolean converted = editor.sector_mode ? EditorSector_Compile (&editor.sectors, &build, error, sizeof (error)) : EditorMap_Clone (&build, &editor.map);
	if (!converted || !Editor_ApplyDefaultWad (&build) ||
		!EditorStaticModels_Prepare (&build,&editor.static_model_lump,&editor.static_model_lump_bytes,error,sizeof(error)) ||
		!EditorMap_Serialize (&build, &data, &length))
	{
		free (editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		EditorMap_Free (&build); Editor_Status (*error ? error : "Could not prepare map for playtest"); return;
	}
	EditorMap_Free (&build);
	qboolean ok = Editor_CreateBuildPaths (data, length); free (data);
	if (!ok) { free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		Editor_Status ("Could not create unique playtest files in maps directory"); return; }
	if (!Editor_StartStage (EDITOR_BUILD_QBSP))
	{
		Sys_remove (editor.source_path); Sys_remove (editor.bsp_path);
		free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		return;
	}
	editor.building_revision = editor.revision;
	Editor_Status ("Compiling map for playtest...");
}
static void Editor_Action (void *ctx, editor_ui_action_t action)
{
	(void)ctx;
	switch (action)
	{
	case EDITOR_UI_SAVE: if (*editor.name) Editor_Save (editor.name); else Editor_Status ("Use editor_save <name> to save this map"); break;
	case EDITOR_UI_PLAY: Editor_Play (); break;
	case EDITOR_UI_UNDO: Editor_UndoRedo (false); break;
	case EDITOR_UI_REDO: Editor_UndoRedo (true); break;
	}
}
static const char *Editor_StatusText (void *ctx) { (void)ctx; return editor.status; }
static void Editor_New_f (void)
{
	if (editor.dirty && (Cmd_Argc () < 2 || strcmp (Cmd_Argv (1), "force")))
	{ Editor_Status ("Unsaved changes; save first or use editor_new force"); return; }
	if (!Editor_NewDocument ()) Editor_Status ("Could not create new map");
}
static void Editor_NewBrush_f (void)
{
	if (editor.dirty && (Cmd_Argc () < 2 || strcmp (Cmd_Argv (1), "force")))
	{ Editor_Status ("Unsaved changes; save first or use editor_new_brush force"); return; }
	if (!Editor_NewBrushDocument ()) Editor_Status ("Could not create brush map");
}
static void Editor_Open_f (void)
{
	if (Cmd_Argc () < 2) { Editor_Status ("Usage: editor_open <name> [force]"); return; }
	if (editor.dirty && (Cmd_Argc () < 3 || strcmp (Cmd_Argv (2), "force")))
	{ Editor_Status ("Unsaved changes; save first or use editor_open <name> force"); return; }
	Editor_Open (Cmd_Argv (1));
}
static void Editor_Save_f (void)
{
	if (!editor.has_map) { Editor_Status ("No map is open"); return; }
	Editor_Save (Cmd_Argc () >= 2 ? Cmd_Argv (1) : editor.name);
}
static void Editor_CleanupLinks_f (void)
{
	if (!editor.has_map || !editor.sector_mode) { Editor_Status ("Open a sector map to clean up links"); return; }
	editor_sector_map_t next; char error[256] = ""; size_t changes = 0;
	if (!Editor_CloneSector (&next)) return;
	if (!EditorSector_CleanupLinks (&next, &changes, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not clean up sector links"); return; }
	if (!changes) { EditorSector_Free (&next); Editor_Status ("No sector link cleanup changes made"); return; }
	Editor_CommitSector (&next); Editor_Status ("Sector links cleaned up");
}
static void Editor_PropVelocity_f (void)
{
	if (!editor.has_map || !editor.sector_mode || editor.selected < 0 || (size_t)editor.selected >= editor.sectors.num_sectors)
	{ Editor_Status ("Select a dynamic or movable sector prop first"); return; }
	const editor_sector_t *sector = &editor.sectors.sectors[editor.selected];
	if (sector->prop_type != EDITOR_SECTOR_PROP_DYNAMIC && sector->prop_type != EDITOR_SECTOR_PROP_MOVABLE)
	{ Editor_Status ("Select a dynamic or movable sector prop first"); return; }
	if (Cmd_Argc () == 1)
	{
		char message[256]; q_snprintf (message, sizeof (message), "Prop velocity: %.3f %.3f %.3f", sector->prop_velocity[0], sector->prop_velocity[1], sector->prop_velocity[2]);
		Editor_Status (message); return;
	}
	if (Cmd_Argc () != 4) { Editor_Status ("Usage: editor_prop_velocity <x> <y> <z>"); return; }
	double velocity[3];
	for (int i = 0; i < 3; ++i)
	{
		char *end = NULL; const char *arg = Cmd_Argv (i + 1);
		velocity[i] = strtod (arg, &end);
		if (!end || end == arg || *end || !isfinite (velocity[i])) { Editor_Status ("Prop velocity must be three finite numbers"); return; }
	}
	if (velocity[0] == sector->prop_velocity[0] && velocity[1] == sector->prop_velocity[1] && velocity[2] == sector->prop_velocity[2])
	{ Editor_Status ("Prop velocity unchanged"); return; }
	editor_sector_map_t next; char error[256] = "";
	if (!Editor_CloneSector (&next)) return;
	if (!EditorSector_SetPropVelocity (&next, (size_t)editor.selected, velocity, error, sizeof (error)))
	{ EditorSector_Free (&next); Editor_Status (*error ? error : "Could not set prop velocity"); return; }
	Editor_CommitSector (&next); Editor_Status ("Prop velocity updated");
}
static void Editor_LightIntensity_f (void)
{
	const editor_map_t *current = Editor_PointMap ();
	if (!Editor_IsLight (current, editor.selected_point)) { Editor_Status ("Select a light first"); return; }
	const char *old = EditorMap_GetProperty (&current->entities[editor.selected_point], "light");
	if (Cmd_Argc () == 1)
	{
		char message[256]; q_snprintf (message, sizeof (message), "Light intensity: %s", old && *old ? old : "300 (default)");
		Editor_Status (message); return;
	}
	if (Cmd_Argc () != 2) { Editor_Status ("Usage: editor_light_intensity <value>"); return; }
	const char *arg = Cmd_Argv (1); char *end = NULL; double value = strtod (arg, &end);
	if (!end || end == arg || *end || !isfinite (value) || fabs (value) > 1000000)
	{ Editor_Status ("Light intensity must be finite and within +/-1000000"); return; }
	char text[64]; q_snprintf (text, sizeof (text), "%.17g", value);
	if (old && !strcmp (old, text)) { Editor_Status ("Light intensity unchanged"); return; }
	editor_sector_map_t sectors; editor_map_t map, *points;
	if (!Editor_ClonePointDocument (&sectors, &map, &points)) return;
	if (!EditorMap_SetProperty (&points->entities[editor.selected_point], "light", text))
	{ Editor_FreePointDocument (&sectors, &map); Editor_Status ("Could not set light intensity"); return; }
	Editor_CommitPointDocument (&sectors, &map); Editor_Status ("Light intensity updated");
}
static void Editor_Model_f (void)
{
	if (Cmd_Argc()!=2) { Editor_Status ("Usage: editor_model <game-relative model path>");return; }
	EditorUI_CreateModel (Cmd_Argv(1));
}
static void Editor_Play_f (void)
{
	if (!editor.has_map) { Editor_Status ("No map is open"); return; }
	Editor_Play ();
}
static void Editor_Script_f (void)
{
	if (!editor.has_map) { Editor_Status ("No map is open"); return; }
	if (Cmd_Argc () == 1) { Con_Printf ("World script: %s\n", Editor_WorldScript (NULL)); return; }
	if (Cmd_Argc () != 2) { Con_Printf ("Usage: editor_script <project-relative .qs path>\n"); return; }
	Editor_SetWorldScript (NULL, Cmd_Argv (1));
}
static void Editor_Return_f (void)
{
	if (!editor.has_map) { Editor_Status ("No map is open; use editor_new"); return; }
	Editor_Enter (); Editor_Status ("Editing source map");
}
qboolean Editor_ToggleKey (qboolean down)
{
	if (COM_CheckParm ("-quiver-editor-playtest"))
	{
		if (down) { key_dest = key_console; Cbuf_AddText ("quit\n"); }
		return true;
	}
	if (!editor.has_map || (!EditorUI_IsActive () && !editor.playtesting))
		return false;
	if (down)
	{
		if (EditorUI_IsActive ()) Editor_Play_f ();
		else Editor_Return_f ();
	}
	return true;
}
void Editor_Init (void)
{
	if (editor.initialized) return;
	editor.initialized = true;
	Cvar_RegisterVariable (&editor_qbsp); Cvar_RegisterVariable (&editor_texture);
	Cmd_AddCommand ("editor_new", Editor_New_f); Cmd_AddCommand ("editor_new_brush", Editor_NewBrush_f); Cmd_AddCommand ("editor_open", Editor_Open_f);
	Cmd_AddCommand ("editor_save", Editor_Save_f); Cmd_AddCommand ("editor_play", Editor_Play_f); Cmd_AddCommand ("editor_return", Editor_Return_f);
	Cmd_AddCommand ("editor_cleanup_links", Editor_CleanupLinks_f);
	Cmd_AddCommand ("editor_prop_velocity", Editor_PropVelocity_f);
	Cmd_AddCommand ("editor_light_intensity", Editor_LightIntensity_f);
	Cmd_AddCommand ("editor_model", Editor_Model_f);
	Cmd_AddCommand ("editor_script", Editor_Script_f);
	editor_ui_adapter_t adapter = {0};
	adapter.brush_count = Editor_BrushCount; adapter.brush_bounds = Editor_Bounds;
	adapter.brush_face_count = Editor_FaceCount; adapter.brush_face_vertices = Editor_FaceVertices;
	adapter.selected = Editor_Selected; adapter.select = Editor_Select; adapter.create_box = Editor_AddBox;
	adapter.point_count = Editor_PointCount; adapter.point_origin = Editor_PointOrigin;
	adapter.point_selected = Editor_PointSelected; adapter.point_select = Editor_PointSelect;
	adapter.point_create_light = Editor_CreateLight; adapter.point_set_origin = Editor_SetPointOrigin;
	adapter.point_light_color = Editor_PointLightColor; adapter.point_set_light_color = Editor_SetPointLightColor;
	adapter.point_remove = Editor_RemovePoint; adapter.duplicate_selected = Editor_DuplicateSelected;
	adapter.point_model=Editor_PointModel;adapter.point_set_model=Editor_SetPointModel;adapter.point_create_model=Editor_CreateModel;
	adapter.point_model_bounds=Editor_PointModelBounds;
	adapter.set_bounds = Editor_SetBounds; adapter.remove_selected = Editor_RemoveSelected;
	adapter.place_spawn = Editor_PlaceSpawn; adapter.spawn_origin = Editor_GetSpawn;
	adapter.action = Editor_Action; adapter.status = Editor_StatusText;
	adapter.sector_mode = Editor_IsSectorMode; adapter.sector_face_id = Editor_SectorFaceId;
	adapter.sector_prop_type = Editor_SectorPropType; adapter.sector_set_prop_type = Editor_SectorSetPropType; adapter.sector_create_prop = Editor_CreateProp;
	adapter.sector_collision_enabled = Editor_SectorCollisionEnabled; adapter.sector_set_collision_enabled = Editor_SectorSetCollisionEnabled;
	adapter.sector_face_portal_peer = Editor_SectorFacePortalPeer;
	adapter.sector_source_face_vertices = Editor_SectorSourceFaceVertices; adapter.sector_knife = Editor_SectorKnife;
	adapter.sector_revision = Editor_SectorRevision;
	adapter.world_script = Editor_WorldScript; adapter.set_world_script = Editor_SetWorldScript;
	adapter.sector_face_is_portal = Editor_SectorFaceIsPortal; adapter.sector_move_face = Editor_SectorMoveFace;
	adapter.sector_vertex_count = Editor_SectorVertexCount; adapter.sector_vertex = Editor_SectorVertex; adapter.sector_set_vertex = Editor_SectorSetVertex;
	adapter.sector_vertex_same_axis = Editor_SectorVertexSameAxis;
	adapter.sector_vertex_coincident = Editor_SectorVertexCoincident; adapter.sector_face_vertex_coincident = Editor_SectorFaceVertexCoincident;
	adapter.sector_same_vertex = Editor_SectorSameVertex; adapter.sector_merge_vertex = Editor_SectorMergeVertex;
	adapter.sector_link_portal = Editor_SectorLink; adapter.sector_unlink_portal = Editor_SectorUnlink; adapter.sector_apply_material = Editor_SectorApplyMaterial;
	EditorUI_SetAdapter (&adapter);
}
void Editor_Frame (void)
{
	if (key_dest == key_game) EditorUI_Update ();
	else EditorUI_CancelDrag ();
	/* Model reload/preferences are processed before this main-thread step.
 * Reacquire exact headers here; the parallel GUI draw only reads them. */
	EditorModelPreview_BeginFrame ();
	if (EditorUI_IsActive ())
	{
		const editor_map_t *points = editor.sector_mode ? &editor.sectors.entities : &editor.map;
		for (size_t i = 1; i < points->num_entities; ++i)
		{
			editor_map_model_t placement;
			if (EditorMap_GetModel (points, i, &placement)) EditorModelPreview_Prepare (&placement);
		}
	}
	if (editor.play_process)
	{
		char output[4097], error[256] = ""; size_t size = 0; uint32_t status = 0;
		editor_process_status_t result = Editor_ProcessPoll (editor.play_process, output, sizeof (output) - 1, &size, &status, error, sizeof (error));
		if (size) { output[size] = 0; Con_Printf ("%s", output); }
		if (result != EDITOR_PROCESS_RUNNING)
		{
			Editor_ProcessDestroy (editor.play_process); editor.play_process = NULL;
			Editor_Status (result == EDITOR_PROCESS_EXITED && !status ? "Scripted playtest ended" : "Scripted playtest failed; see console");
		}
	}
	if (!editor.process) return;
	char output[4097], error[256] = ""; size_t size = 0; uint32_t exit_code = 0;
	editor_process_status_t result = Editor_ProcessPoll (editor.process, output, sizeof (output) - 1, &size, &exit_code, error, sizeof (error));
	if (size) { output[size] = 0; Con_Printf ("%s", output); }
	if (result == EDITOR_PROCESS_RUNNING) return;
	Editor_ProcessDestroy (editor.process); editor.process = NULL;
	if (result == EDITOR_PROCESS_ERROR || exit_code != 0 || !Editor_PlaytestOutputExists ())
	{
		Sys_remove (editor.source_path); Sys_remove (editor.bsp_path);
		free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		Editor_Status (editor.build_stage == EDITOR_BUILD_QBSP ? "QBSP failed; see console for compiler diagnostics" :
			editor.build_stage == EDITOR_BUILD_VIS ? "VIS failed; see console for compiler diagnostics" :
			editor.build_stage == EDITOR_BUILD_SCRIPT ? "Script compilation failed; see console for compiler diagnostics" : "LIGHT failed; see console for compiler diagnostics");
		if (*error) Con_Printf ("%s\n", error);
		return;
	}
	if (editor.building_revision != editor.revision)
	{
		Sys_remove (editor.source_path); Sys_remove (editor.bsp_path);
		free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		Editor_Status ("Map changed while compiling; play again to build the latest version"); return;
	}
	if (editor.build_stage == EDITOR_BUILD_LIGHT)
	{
		if (!EditorStaticModels_Attach (editor.bsp_path,editor.static_model_lump,editor.static_model_lump_bytes,error,sizeof(error)))
		{
			Sys_remove(editor.source_path);Sys_remove(editor.bsp_path);
			free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
			Editor_Status(*error?error:"Could not attach static model geometry");return;
		}
		free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
	}
	if (editor.build_stage == EDITOR_BUILD_LIGHT && *editor.script_path)
	{
		if (!Editor_StartScriptBuild ()) { Sys_remove (editor.source_path); Sys_remove (editor.bsp_path); }
		return;
	}
	if (editor.build_stage < EDITOR_BUILD_LIGHT)
	{
		if (!Editor_StartStage ((editor_build_stage_t)(editor.build_stage + 1)))
		{
			Sys_remove (editor.source_path); Sys_remove (editor.bsp_path);
			free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
		}
		return;
	}
	editor.compiled_revision = editor.building_revision;
	Editor_LaunchPlaytest ();
}
void Editor_Shutdown (void)
{
	EditorModelPreview_Clear ();
	if (editor.process) { Editor_ProcessCancel (editor.process); Editor_ProcessDestroy (editor.process); editor.process = NULL; }
	if (editor.play_process) { Editor_ProcessCancel (editor.play_process); Editor_ProcessDestroy (editor.play_process); editor.play_process = NULL; }
	EditorUI_SetActive (false); EditorUI_SetAdapter (NULL);
	free(editor.static_model_lump);editor.static_model_lump=NULL;editor.static_model_lump_bytes=0;
	Editor_FreeGeometry ();
	free(editor.model_bounds);editor.model_bounds=NULL;editor.model_bounds_count=0;
	EditorSector_Free (&editor.sectors); Editor_FreeSectorHistory (editor.sector_undo, &editor.nsector_undo); Editor_FreeSectorHistory (editor.sector_redo, &editor.nsector_redo);
	EditorMap_Free (&editor.map); Editor_FreeHistory (editor.undo, &editor.nundo); Editor_FreeHistory (editor.redo, &editor.nredo);
	memset (&editor, 0, sizeof (editor)); editor.selected = editor.selected_point = -1;
}
