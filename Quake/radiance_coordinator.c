#include "quakedef.h"
#include "radiance_coordinator.h"
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Only these fields affect grid membership. Intensities, colors, gains and
 * lightstyles can change without rebuilding the conservative index lists. */
typedef struct
{
	float position_radius[4], direction_cone[4], width, compiler_marker;
	uint32_t type, flags, formula;
} radiance_grid_key_t;

/* A revision is unique across coordinator/map lifetimes, so an uploader
 * retaining old frame slots can never mistake a new grid for an old one.
 * Saturation disables caching instead of reusing an identity. */
static atomic_uint64_t radiance_grid_revision_counter;
static uint64_t RadianceCoordinator_NextGridRevision (void)
{
	uint64_t previous = Atomic_LoadUInt64 (&radiance_grid_revision_counter);
	for (;;)
	{
		if (previous == UINT64_MAX) return 0;
		if (Atomic_CompareExchangeUInt64 (&radiance_grid_revision_counter, &previous, previous + 1)) return previous + 1;
	}
}

static radiance_grid_key_t RadianceCoordinator_GridKey (const radiance_light_t *light)
{
	radiance_grid_key_t key = {0};
	memcpy (key.position_radius, light->position_radius, sizeof (key.position_radius));
	memcpy (key.direction_cone, light->direction_cone, sizeof (key.direction_cone));
	key.width = light->area_axis[3]; key.compiler_marker = light->area_axis[2];
	key.type = light->metadata[0]; key.flags = light->metadata[2]; key.formula = light->metadata[3];
	return key;
}

static bool RadianceCoordinator_GridMatches (const radiance_coordinator_t *c, const radiance_light_grid_t *grid)
{
	if (!c->grid_valid || c->cached_key_count != c->frame_count || c->cached_index_count > c->limits.max_grid_indices ||
		memcmp (c->cached_grid.origin, grid->origin, sizeof (grid->origin)) ||
		memcmp (c->cached_grid.cell_size, grid->cell_size, sizeof (grid->cell_size)) ||
		memcmp (c->cached_grid.dimensions, grid->dimensions, sizeof (grid->dimensions))) return false;
	const radiance_grid_key_t *keys = c->grid_keys;
	for (size_t i = 0; i < c->frame_count; ++i)
	{
		radiance_grid_key_t key = RadianceCoordinator_GridKey (&c->frame_lights[i]);
		if (memcmp (&keys[i], &key, sizeof (key)))
		{
			/* Only spotlights clip their bounds by orientation. For every
			 * other source an existing superset of cells remains conservative,
			 * even if a dynamic light moves or decays inside those cells. */
			radiance_light_t old = {0};
			memcpy (old.position_radius, keys[i].position_radius, sizeof (old.position_radius));
			memcpy (old.direction_cone, keys[i].direction_cone, sizeof (old.direction_cone));
			old.area_axis[3] = keys[i].width;
			old.metadata[0] = keys[i].type; old.metadata[2] = keys[i].flags; old.metadata[3] = keys[i].formula;
			if (!Radiance_LightGridBoundsCover (grid, &old, &c->frame_lights[i])) return false;
		}
	}
	return true;
}

static void RadianceCoordinator_ResetMap (radiance_coordinator_t *c)
{
	RadianceQuakeMaterials_Clear (&c->materials);
	RadianceQuakeMaterials_Init (&c->materials);
	RadianceMaterialAtlas_Clear (&c->atlas);
	free (c->map_lights);
	free (c->surface_templates); free (c->surface_static_lights); free (c->surface_dynamic_lights);
	free (c->surface_static_matches); free (c->surface_dynamic_matches);
	c->surface_templates = NULL; c->surface_static_lights = c->surface_dynamic_lights = NULL;
	c->surface_static_matches = c->surface_dynamic_matches = NULL;
	c->surface_template_count = c->surface_static_count = c->surface_dynamic_count = 0;
	c->surface_unmatched_count = 0;
	c->surface_static_valid = c->surface_dynamic_valid = false;
	c->map_lights = NULL; c->map_count = 0; c->map_loaded = false; c->atlas_valid = false; c->grid_valid = false;
	memset (&c->map_stats, 0, sizeof (c->map_stats));
}

bool RadianceCoordinator_InitLights (radiance_coordinator_t *c, const radiance_coordinator_limits_t *limits)
{
	if (!c || !limits || !limits->max_map_lights || !limits->max_lights ||
		limits->max_map_lights > limits->max_lights || limits->max_lights > UINT32_MAX ||
		limits->max_lights > SIZE_MAX / sizeof (radiance_light_t) || !limits->max_grid_cells) return false;
	memset (c, 0, sizeof (*c));
	c->limits = *limits;
	c->point_gain = 1.0f; c->emissive_gain = 1.0f;
	RadianceQuakeMaterials_Init (&c->materials);
	c->frame_lights = malloc (limits->max_lights * sizeof (*c->frame_lights));
	c->grid_keys = malloc (limits->max_lights * sizeof (radiance_grid_key_t));
	if (!c->frame_lights || !c->grid_keys) { free (c->frame_lights); free (c->grid_keys); memset (c, 0, sizeof (*c)); return false; }
	return true;
}

bool RadianceCoordinator_SetGains (radiance_coordinator_t *c, float point_gain, float emissive_gain)
{
	if (!c || !c->frame_lights || !isfinite(point_gain) || point_gain < 0 || !isfinite(emissive_gain) || emissive_gain < 0) return false;
	c->point_gain=point_gain; c->emissive_gain=emissive_gain; return true;
}

bool RadianceCoordinator_SetSpotSoftness (radiance_coordinator_t *c, float softness)
{
	if (!c || !c->frame_lights || !isfinite (softness)) return false;
	c->spot_softness = fminf (1.0f, fmaxf (0.0f, softness));
	return true;
}

void RadianceCoordinator_SetCooperative (radiance_coordinator_t *c, bool cooperative)
{
	if (c) c->cooperative = cooperative;
}

void RadianceCoordinator_SetViewOrigin (radiance_coordinator_t *c, const float origin[3])
{
	if (!c) return;
	c->view_origin_valid = origin && isfinite (origin[0]) && isfinite (origin[1]) && isfinite (origin[2]);
	if (c->view_origin_valid) memcpy (c->view_origin, origin, sizeof (c->view_origin));
}

bool RadianceCoordinator_Init (radiance_coordinator_t *c, radiance_gpu_inputs_t *gpu,
	const radiance_coordinator_limits_t *limits)
{
	if (!gpu || !limits || !limits->max_atlas_dimension || !limits->max_atlas_layers || !limits->max_atlas_bytes ||
		!RadianceCoordinator_InitLights (c, limits)) return false;
	c->gpu = gpu;
	return true;
}

void RadianceCoordinator_Clear (radiance_coordinator_t *c)
{
	if (!c) return;
	RadianceCoordinator_ResetMap (c);
	free (c->frame_lights); free (c->grid_offsets); free (c->grid_indices); free (c->grid_keys);
	memset (c, 0, sizeof (*c));
}

bool RadianceCoordinator_LoadMap (radiance_coordinator_t *c, const char *entities, size_t length)
{
	if (!c || !c->frame_lights || (!entities && length)) return false;
	RadianceCoordinator_ResetMap (c);
	c->map_lights = calloc (c->limits.max_map_lights, sizeof (*c->map_lights));
	c->surface_templates = calloc (c->limits.max_map_lights, sizeof (*c->surface_templates));
	if (!c->map_lights || !c->surface_templates) return false;
	if (!Radiance_ParseMapLightsWithSurfaceTemplates (entities, length, c->map_lights, c->limits.max_map_lights,
		c->surface_templates, c->limits.max_map_lights, &c->surface_template_count, &c->map_stats) ||
		c->map_stats.dropped) return false;
	c->map_count = c->map_stats.written; c->map_loaded = true;
	return true;
}

typedef struct
{
	float start[3], end[3];
} radiance_surface_edge_t;

typedef struct
{
	uint32_t material, face, instance, template_index;
	double normal[3];
	radiance_surface_edge_t *edges;
	size_t edge_count, edge_capacity;
} radiance_surface_group_t;

static bool RadianceCoordinator_TextureEqual (const char *a, const char *b)
{
	while (*a && *b && tolower ((unsigned char)*a) == tolower ((unsigned char)*b)) { ++a; ++b; }
	return !*a && !*b;
}

static bool RadianceCoordinator_VertexEqual (const float a[3], const float b[3])
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* Cancel interior triangle edges to recover each original polygon boundary. */
static bool RadianceCoordinator_SurfaceEdge (radiance_surface_group_t *g, const float *a, const float *b)
{
	if (RadianceCoordinator_VertexEqual (a, b)) return true;
	for (size_t i = 0; i < g->edge_count; ++i)
		if (RadianceCoordinator_VertexEqual (g->edges[i].start, b) && RadianceCoordinator_VertexEqual (g->edges[i].end, a))
		{
			g->edges[i] = g->edges[--g->edge_count];
			return true;
		}
	if (g->edge_count == g->edge_capacity)
	{
		size_t capacity = g->edge_capacity ? g->edge_capacity * 2 : 16;
		/* Original compiler accepts at most 60 vertices per polygon. More
		 * boundary edges would require an unbounded malformed face. */
		if (capacity > 1024) return false;
		void *p = realloc (g->edges, capacity * sizeof (*g->edges));
		if (!p) return false;
		g->edges = p; g->edge_capacity = capacity;
	}
	memcpy (g->edges[g->edge_count].start, a, sizeof (g->edges[0].start));
	memcpy (g->edges[g->edge_count++].end, b, sizeof (g->edges[0].end));
	return true;
}

/* ericw's GL_SubdivideSurface: split at the nearest 128-unit axis plane,
 * leaving at least 8 units on either side; average vertices at each leaf. */
static bool RadianceCoordinator_SubdivideSurface (const radiance_surface_template_t *t,
	const double normal[3], const float (*vertices)[3], unsigned count, unsigned depth,
	radiance_light_t *out, size_t capacity, size_t *written)
{
	if (count < 3 || count > 60 || depth > 128) return false;
	for (int axis = 0; axis < 3; ++axis)
	{
		float low=vertices[0][axis], high=low;
		for (unsigned i=0; i<count; ++i) { low=fminf(low,vertices[i][axis]); high=fmaxf(high,vertices[i][axis]); }
		float plane = 128.0f * floorf (((low + high) * .5f) / 128.0f + .5f);
		if (!isfinite(plane)) return false;
		if (high-plane < 8 || plane-low < 8) continue;
		float front[64][3], back[64][3]; unsigned nf=0, nb=0;
		for (unsigned i=0; i<count; ++i)
		{
			const float *a=vertices[i], *b=vertices[(i+1)%count];
			float da=a[axis]-plane, db=b[axis]-plane;
			if (da>=0) { if(nf>=64) return false; memcpy(front[nf++],a,3*sizeof(float)); }
			if (da<=0) { if(nb>=64) return false; memcpy(back[nb++],a,3*sizeof(float)); }
			if (da!=0 && db!=0 && ((da>0)!=(db>0)))
			{
				if(nf>=64 || nb>=64) return false;
				float fraction=da/(da-db);
				for(int j=0;j<3;++j) front[nf][j]=back[nb][j]=a[j]+fraction*(b[j]-a[j]);
				++nf; ++nb;
			}
		}
		return RadianceCoordinator_SubdivideSurface(t,normal,front,nf,depth+1,out,capacity,written) &&
			RadianceCoordinator_SubdivideSurface(t,normal,back,nb,depth+1,out,capacity,written);
	}
	if (*written==capacity) return false;
	radiance_light_t *light=&out[(*written)++]; *light=t->light;
	for(int axis=0;axis<3;++axis)
	{
		double sum=0;
		for(unsigned i=0;i<count;++i) sum+=vertices[i][axis];
		light->position_radius[axis]=(float)(sum/count+t->offset*normal[axis]);
		if(t->surface_spotlight) light->direction_cone[axis]=(float)normal[axis];
	}
	if(t->surface_spotlight)
	{
		if(light->metadata[0]!=RADIANCE_LIGHT_SPOT)
		{
			light->direction_cone[3]=cosf(20.0f*0.01745329251994329577f);
			light->area_axis[1]=light->direction_cone[3];
		}
		light->metadata[0]=RADIANCE_LIGHT_SPOT;
	}
	return true;
}

static bool RadianceCoordinator_BuildSurfacePart (const radiance_coordinator_t *c,
	const rc_scene_triangle_t *triangles, uint32_t triangle_count, const radiance_quake_materials_t *materials,
	radiance_light_t **lights_out, size_t *count_out, uint8_t **matches_out)
{
	size_t max=c->limits.max_lights, table_size=1, count=0, written=0;
	if (max>SIZE_MAX/2) return false;
	while(table_size<max*2) { if(table_size>SIZE_MAX/2) return false; table_size*=2; }
	radiance_surface_group_t *groups=calloc(max,sizeof(*groups));
	uint32_t *slots=calloc(table_size,sizeof(*slots));
	uint8_t *matches=calloc(c->surface_template_count,1);
	radiance_light_t *lights=malloc(max*sizeof(*lights));
	if(!groups || !slots || !matches || !lights) {free(groups);free(slots);free(matches);free(lights);return false;}
	bool ok=false;
	for(uint32_t i=0;i<triangle_count;++i)
	{
		const rc_scene_triangle_t *tri=&triangles[i]; uint32_t material=tri->metadata[0];
		if((tri->metadata[1]&RC_SCENE_SKY) || material>=materials->count || !materials->entries[material].brush) continue;
		const char *name=materials->entries[material].brush->name;
		for(size_t t=0;t<c->surface_template_count;++t)
		{
			if(!RadianceCoordinator_TextureEqual(name,c->surface_templates[t].texture)) continue;
			double ab[3],ac[3],normal[3];
			for(int a=0;a<3;++a) { ab[a]=tri->position[1][a]-tri->position[0][a]; ac[a]=tri->position[2][a]-tri->position[0][a]; }
			normal[0]=ab[1]*ac[2]-ab[2]*ac[1]; normal[1]=ab[2]*ac[0]-ab[0]*ac[2]; normal[2]=ab[0]*ac[1]-ab[1]*ac[0];
			double area=sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
			if(!isfinite(area)) goto done;
			if(area<2e-9) continue;
			uint32_t face=tri->metadata[2],instance=tri->metadata[3];
			uint64_t hash=(uint64_t)material*0x9e3779b185ebca87ULL^(uint64_t)face*0xc2b2ae3d27d4eb4fULL^
				(uint64_t)instance*0x165667b19e3779f9ULL^(uint64_t)t*0x85ebca77c2b2ae63ULL;
			size_t slot=(size_t)hash&(table_size-1);
			while(slots[slot]) {radiance_surface_group_t *g=&groups[slots[slot]-1]; if(g->material==material && g->face==face && g->instance==instance && g->template_index==t) break; slot=(slot+1)&(table_size-1);}
			if(!slots[slot])
			{
				if(count==max) goto done;
				groups[count].material=material; groups[count].face=face; groups[count].instance=instance; groups[count].template_index=(uint32_t)t;
				slots[slot]=(uint32_t)++count;
			}
			radiance_surface_group_t *g=&groups[slots[slot]-1];
			for(int a=0;a<3;++a) g->normal[a]+=normal[a];
			for(int edge=0;edge<3;++edge)
				if(!RadianceCoordinator_SurfaceEdge(g,tri->position[edge],tri->position[(edge+1)%3])) goto done;
		}
	}
	for(size_t i=0;i<count;++i)
	{
		radiance_surface_group_t *g=&groups[i];
		double len=sqrt(g->normal[0]*g->normal[0]+g->normal[1]*g->normal[1]+g->normal[2]*g->normal[2]);
		if(!isfinite(len)||len<1e-9) goto done;
		double normal[3]={g->normal[0]/len,g->normal[1]/len,g->normal[2]/len};
		while(g->edge_count)
		{
			float vertices[60][3]; unsigned n=0;
			float first[3],next[3]; memcpy(first,g->edges[0].start,sizeof(first)); memcpy(next,first,sizeof(next));
			do {
				if(n==60) goto done;
				memcpy(vertices[n++],next,sizeof(next)); size_t edge=0;
				while(edge<g->edge_count && !RadianceCoordinator_VertexEqual(g->edges[edge].start,next)) ++edge;
				if(edge==g->edge_count) goto done;
				memcpy(next,g->edges[edge].end,sizeof(next)); g->edges[edge]=g->edges[--g->edge_count];
			} while(!RadianceCoordinator_VertexEqual(next,first));
			if(!RadianceCoordinator_SubdivideSurface(&c->surface_templates[g->template_index],normal,vertices,n,0,lights,max,&written)) goto done;
		}
		matches[g->template_index]=1;
	}
	if (!written) { free(lights); lights=NULL; }
	else {
		void *smaller=realloc(lights,written*sizeof(*lights));
		if(smaller) lights=smaller;
	}
	*lights_out=lights; *count_out=written; *matches_out=matches; lights=NULL; matches=NULL; ok=true;
done:
	for(size_t i=0;i<count;++i) free(groups[i].edges);
	free(groups); free(slots); free(matches); free(lights); return ok;
}

bool RadianceCoordinator_PrepareSurfaceLights (radiance_coordinator_t *c, const rc_scene_t *scene,
	uint32_t static_count, const radiance_quake_materials_t *materials, uint64_t geometry_generation)
{
	if (!c || !c->map_loaded || !scene || !materials || static_count > scene->triangle_count ||
		(scene->triangle_count && !scene->triangles)) return false;
	if (!c->surface_template_count) return true;
	if (!c->surface_static_valid)
	{
		radiance_light_t *lights = NULL; size_t count = 0; uint8_t *matches = NULL;
		if (!RadianceCoordinator_BuildSurfacePart (c, scene->triangles, static_count, materials, &lights, &count, &matches)) return false;
		free (c->surface_static_lights); c->surface_static_lights = lights; c->surface_static_count = count; c->surface_static_valid = true;
		free (c->surface_static_matches); c->surface_static_matches = matches;
	}
	if (!c->surface_dynamic_valid || c->surface_geometry_generation != geometry_generation)
	{
		radiance_light_t *lights = NULL; size_t count = 0; uint8_t *matches = NULL; uint32_t n = scene->triangle_count - static_count;
		if (!RadianceCoordinator_BuildSurfacePart (c, n ? scene->triangles + static_count : NULL, n, materials, &lights, &count, &matches)) return false;
		free (c->surface_dynamic_lights); c->surface_dynamic_lights = lights; c->surface_dynamic_count = count;
		free (c->surface_dynamic_matches); c->surface_dynamic_matches = matches;
		c->surface_dynamic_valid = true; c->surface_geometry_generation = geometry_generation;
	}
	c->surface_unmatched_count = 0;
	for (size_t i = 0; i < c->surface_template_count; ++i)
		if (!c->surface_static_matches[i] && !c->surface_dynamic_matches[i]) ++c->surface_unmatched_count;
	return true;
}

static bool RadianceCoordinator_Reserve (uint32_t **ptr, size_t *capacity, size_t count)
{
	if (count <= *capacity) return true;
	if (count > SIZE_MAX / sizeof (**ptr)) return false;
	void *replacement = realloc (*ptr, count * sizeof (**ptr));
	if (!replacement) return false;
	*ptr = replacement; *capacity = count;
	return true;
}

bool RadianceCoordinator_PrepareLightsWithEmitters (radiance_coordinator_t *c,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid, const radiance_light_t *emitters, size_t emitter_count,
	radiance_input_snapshot_t *snapshot)
{
	if (snapshot) memset (snapshot, 0, sizeof (*snapshot));
	if (!c || !snapshot || !c->map_loaded || !grid || (!styles && style_count) || (!dynamic_lights && dynamic_count) ||
		(!emitters && emitter_count) || !isfinite (time)) return false;
	c->frame_count = c->map_count;
	for (size_t i = 0; i < c->map_count; ++i)
	{
		c->frame_lights[i] = Radiance_ApplyLightstyle (&c->map_lights[i], styles, style_count);
		if (c->cooperative && (c->frame_lights[i].metadata[2] & RADIANCE_LIGHT_SOURCE_NOT_IN_COOP))
			c->frame_lights[i].color_intensity[3] = 0;
		if (c->view_origin_valid && (c->frame_lights[i].metadata[2] & RADIANCE_LIGHT_SOURCE_KEX))
		{
			const float start = c->frame_lights[i].area_axis[0], end = c->frame_lights[i].area_axis[1];
			if (end > 0.0f)
			{
				double dx = (double)c->frame_lights[i].position_radius[0] - c->view_origin[0];
				double dy = (double)c->frame_lights[i].position_radius[1] - c->view_origin[1];
				double dz = (double)c->frame_lights[i].position_radius[2] - c->view_origin[2];
				double distance = sqrt (dx * dx + dy * dy + dz * dz);
				if (distance >= end) c->frame_lights[i].color_intensity[3] = 0.0f;
				else if (distance > start && end > start)
					c->frame_lights[i].color_intensity[3] *= (float)((end - distance) / (end - start));
			}
		}
		Radiance_ApplyDefaultSpotSoftness (&c->frame_lights[i], c->spot_softness);
	}
	if (c->surface_template_count && (!c->surface_static_valid || !c->surface_dynamic_valid)) return false;
	if (c->surface_static_count > c->limits.max_lights - c->frame_count ||
		c->surface_dynamic_count > c->limits.max_lights - c->frame_count - c->surface_static_count) return false;
	for (size_t part = 0; part < 2; ++part)
	{
		const radiance_light_t *source = part ? c->surface_dynamic_lights : c->surface_static_lights;
		size_t count = part ? c->surface_dynamic_count : c->surface_static_count;
		for (size_t i = 0; i < count; ++i)
		{
			c->frame_lights[c->frame_count] = Radiance_ApplyLightstyle (&source[i], styles, style_count);
			Radiance_ApplyDefaultSpotSoftness (&c->frame_lights[c->frame_count], c->spot_softness);
			++c->frame_count;
		}
	}
	for (size_t i = 0; i < dynamic_count; ++i)
	{
		const dlight_t *d = &dynamic_lights[i];
		/* R_UpdateEntityDlights mirrors BSP dynamiclight records under these
		 * keys. The map records are already in map_lights. */
		if (d->key >= 0x40000000 && d->key < 0x40000040) continue;
		radiance_light_t light;
		float intensity = d->kex_intensity > 0 ? d->kex_intensity : 1.0f;
		if (!Radiance_MakeDynamicLight (&light, d->origin, d->radius, d->color, intensity,
			d->cone_dir, d->cone_cos, d->die, time)) continue;
		if (d->kex_intensity <= 0) Radiance_ConfigureLegacyDynamicLight (&light, d->minlight);
		else light.metadata[2] |= RADIANCE_LIGHT_SOURCE_KEX;
		if (c->frame_count == c->limits.max_lights) return false;
		c->frame_lights[c->frame_count++] = light;
	}
	if (emitter_count > c->limits.max_lights - c->frame_count) return false;
	for (size_t i=0; i<emitter_count; ++i)
	{
		if (emitters[i].metadata[0] != RADIANCE_LIGHT_EMISSIVE || !(emitters[i].metadata[2] & RADIANCE_LIGHT_SOURCE_TEXTURE)) return false;
		c->frame_lights[c->frame_count++] = emitters[i];
	}
	size_t cells = 1;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!grid->dimensions[axis] || cells > c->limits.max_grid_cells / grid->dimensions[axis]) return false;
		cells *= grid->dimensions[axis];
	}
	if (cells >= UINT32_MAX || !RadianceCoordinator_Reserve (&c->grid_offsets, &c->offset_capacity, cells + 1)) return false;
	radiance_light_grid_stats_t stats = {0};
	if (RadianceCoordinator_GridMatches (c, grid)) stats.required_indices = c->cached_index_count;
	else
	{
		c->grid_valid = false;
		/* Moving lights can invalidate the grid every frame. Reuse the previous
		 * index allocation for the first build, and retry only when it is too
		 * small. The builder reports the required count even on that failure. */
		bool built = Radiance_BuildLightGrid (grid, c->frame_lights, c->frame_count, c->grid_offsets,
			c->offset_capacity, c->grid_indices, c->index_capacity, &stats);
		if (stats.required_indices > c->limits.max_grid_indices) return false;
		if (!built)
		{
			if (!stats.required_indices || stats.required_indices <= c->index_capacity ||
				!RadianceCoordinator_Reserve (&c->grid_indices, &c->index_capacity, stats.required_indices) ||
				!Radiance_BuildLightGrid (grid, c->frame_lights, c->frame_count, c->grid_offsets,
					c->offset_capacity, c->grid_indices, c->index_capacity, &stats)) return false;
		}
		radiance_grid_key_t *keys = c->grid_keys;
		for (size_t i = 0; i < c->frame_count; ++i) keys[i] = RadianceCoordinator_GridKey (&c->frame_lights[i]);
		c->cached_grid = *grid; c->cached_key_count = c->frame_count; c->cached_index_count = stats.required_indices;
		c->grid_revision = RadianceCoordinator_NextGridRevision ();
		c->grid_valid = true; ++c->grid_builds;
	}
	snapshot->lights = c->frame_lights; snapshot->light_count = c->frame_count;
	snapshot->grid_offsets = c->grid_offsets; snapshot->grid_offset_count = cells + 1;
	snapshot->grid_indices = c->grid_indices; snapshot->grid_index_count = stats.required_indices;
	snapshot->grid_revision = c->grid_revision;
	snapshot->parameters.counts[0] = (uint32_t)c->frame_count;
	snapshot->parameters.counts[3] = 1 | 4;
	snapshot->parameters.grid_origin[3] = c->point_gain;
	snapshot->parameters.grid_inv_cell_size[3] = c->emissive_gain;
	memcpy (snapshot->parameters.sky_radiance, c->map_stats.sky_upper, sizeof (c->map_stats.sky_upper));
	memcpy (snapshot->parameters.ground_radiance, c->map_stats.sky_lower, sizeof (c->map_stats.sky_lower));
	for (int axis = 0; axis < 3; ++axis)
	{
		snapshot->parameters.grid_origin[axis] = grid->origin[axis];
		snapshot->parameters.grid_inv_cell_size[axis] = 1.0f / grid->cell_size[axis];
		snapshot->parameters.grid_dimensions[axis] = grid->dimensions[axis];
	}
	snapshot->parameters.grid_dimensions[3] = (uint32_t)stats.required_indices;
	return true;
}

bool RadianceCoordinator_PrepareLights (radiance_coordinator_t *c,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid, radiance_input_snapshot_t *snapshot)
{
	return RadianceCoordinator_PrepareLightsWithEmitters (c, styles, style_count, dynamic_lights, dynamic_count,
		time, grid, NULL, 0, snapshot);
}

bool RadianceCoordinator_Prepare (radiance_coordinator_t *c, uint32_t frame,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid)
{
	if (!c || !c->gpu) return false;
	radiance_input_snapshot_t snapshot;
	if (!RadianceCoordinator_PrepareLights (c, styles, style_count, dynamic_lights, dynamic_count, time, grid, &snapshot)) return false;
	uint64_t revision = RadianceQuakeMaterials_Revision (&c->materials);
	if (!c->atlas_valid || revision != c->atlas_revision)
	{
		if (!RadianceQuakeMaterials_BuildAtlas (&c->materials, &c->atlas, c->limits.max_atlas_dimension,
			c->limits.max_atlas_layers, c->limits.max_atlas_bytes)) { c->atlas_valid = false; return false; }
		c->atlas_revision = revision; c->atlas_valid = true;
	}
	if (c->atlas.material_count > UINT32_MAX) return false;
	snapshot.materials = c->atlas.materials; snapshot.material_count = c->atlas.material_count;
	snapshot.atlas_rgba = c->atlas.rgba; snapshot.atlas_width = c->atlas.width;
	snapshot.atlas_height = c->atlas.height; snapshot.atlas_layers = c->atlas.layers;
	snapshot.parameters.counts[1] = (uint32_t)c->atlas.material_count;
	snapshot.parameters.counts[2] = c->atlas.layers;
	return RadianceInputs_Update (c->gpu, frame, &snapshot);
}
