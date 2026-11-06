#include "radiance_emissive.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint32_t material, surface, instance, dynamic;
	double area, center[3], normal[3], tangent[3], extents[4], score;
	uint32_t order;
	bool selected, one_sided;
} rc_emit_group_t;
typedef struct { double score; uint32_t index; } rc_emit_rank_t;
typedef struct { uint32_t material, surface, instance, dynamic; } rc_emit_key_t;

static uint64_t RCEmit_Hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint64_t h=UINT64_C(1469598103934665603);
	uint32_t values[4]={a,b,c,d};
	for (int i=0;i<4;++i) { h^=values[i]; h*=UINT64_C(1099511628211); }
	return h;
}
static int RCEmit_Order(const void *a, const void *b)
{
	const rc_emit_rank_t *x=a,*y=b;
	if (x->score > y->score) return -1;
	if (x->score < y->score) return 1;
	return (x->index > y->index) - (x->index < y->index);
}
static double RCEmit_Linear(uint8_t value)
{
	double s=value/255.0;
	return s <= .04045 ? s/12.92 : pow((s+.055)/1.055,2.4);
}
static bool RCEmit_Material(const radiance_material_atlas_t *a, size_t index, double rgb[3])
{
	const radiance_material_t *m=&a->materials[index];
	for(int c=0;c<3;++c) rgb[c]=0;
	if (!(m->layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE)) return true;
	uint32_t x=m->emission_rect[0], y=m->emission_rect[1], w=m->emission_rect[2], h=m->emission_rect[3], layer=m->layers_flags[1];
	if (!a->rgba || !w || !h || layer>=a->layers || x>a->width || w>a->width-x || y>a->height || h>a->height-y) return false;
	uint64_t needed=(uint64_t)a->width*a->height;
	if (a->layers && needed>UINT64_MAX/a->layers) return false;
	needed*=a->layers;
	if (needed>UINT64_MAX/4) return false;
	needed*=4;
	if (needed>a->bytes) return false;
	for(uint32_t row=0;row<h;++row) for(uint32_t col=0;col<w;++col) {
		size_t offset=(((size_t)layer*a->height+y+row)*a->width+x+col)*4;
		const uint8_t *pixel=a->rgba+offset; double alpha=pixel[3]/255.0;
		for(int c=0;c<3;++c) rgb[c]+=RCEmit_Linear(pixel[c])*alpha;
	}
	for(int c=0;c<3;++c) rgb[c]=rgb[c]/((double)w*h)*fmax(0,m->emission[c])*fmax(0,m->emission[3]);
	return true;
}
static bool RCEmit_BuildWithColors(rc_scene_t *s, const radiance_material_atlas_t *a,
	const double (*colors)[3], const float camera[3], radiance_light_t *out, rc_emit_key_t *keys,
	size_t capacity, radiance_emissive_stats_t *stats, bool *changed)
{
	if (stats) memset(stats,0,sizeof(*stats));
	if (changed) *changed=false;
	if (!s || !a || !camera || (capacity && !out) || (s->triangle_count && !s->triangles) ||
		(a->material_count && !a->materials) || a->material_count>UINT32_MAX) return false;
	for(int c=0;c<3;++c) if(!isfinite(camera[c])) return false;
	rc_emit_group_t *groups=NULL; rc_emit_rank_t *ranks=NULL; uint32_t *slots=NULL; bool ok=false;
	if(!colors) goto done;
	size_t count=0;
	for(uint32_t i=0;i<s->triangle_count;++i) {
		const rc_scene_triangle_t *t=&s->triangles[i]; uint32_t m=t->metadata[0];
		if(m>=a->material_count) goto done;
		if(!(t->metadata[1] & RC_SCENE_SKY) &&
			(colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) ++count;
	}
	if(count>UINT32_MAX || count>SIZE_MAX/sizeof(*groups) || count>SIZE_MAX/2) goto done;
	size_t slot_count=1; while(slot_count<count*2) { if(slot_count>SIZE_MAX/2) goto done; slot_count*=2; }
	if(slot_count>SIZE_MAX/sizeof(*slots)) goto done;
	groups=calloc(count ? count : 1,sizeof(*groups)); ranks=calloc(count ? count : 1,sizeof(*ranks)); slots=calloc(slot_count,sizeof(*slots));
	if(!groups||!ranks||!slots) goto done;
	size_t group_count=0;
	for(uint32_t i=0;i<s->triangle_count;++i) {
		const rc_scene_triangle_t *t=&s->triangles[i]; uint32_t m=t->metadata[0];
		if((t->metadata[1]&RC_SCENE_SKY) || !(colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) continue;
		double a0[3],b0[3],cross[3];
		for(int j=0;j<3;++j) { a0[j]=t->position[1][j]-t->position[0][j]; b0[j]=t->position[2][j]-t->position[0][j]; }
		cross[0]=a0[1]*b0[2]-a0[2]*b0[1]; cross[1]=a0[2]*b0[0]-a0[0]*b0[2]; cross[2]=a0[0]*b0[1]-a0[1]*b0[0];
		double area=.5*sqrt(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]);
		if(!isfinite(area)) goto done;
		if(area<=1e-9) continue;
		uint32_t surface=t->metadata[2], instance=t->metadata[3], dynamic=t->metadata[1]&RC_SCENE_DYNAMIC;
		size_t slot=(size_t)RCEmit_Hash(m,surface,instance,dynamic)&(slot_count-1);
		while(slots[slot]) { rc_emit_group_t *g=&groups[slots[slot]-1];
			if(g->material==m && g->surface==surface && g->instance==instance && g->dynamic==dynamic) break;
			slot=(slot+1)&(slot_count-1);
		}
		if(!slots[slot]) { if(group_count>=UINT32_MAX) goto done; slots[slot]=(uint32_t)++group_count;
			groups[group_count-1]=(rc_emit_group_t){.material=m,.surface=surface,.instance=instance,.dynamic=dynamic,.order=(uint32_t)(group_count-1),.one_sided=true};
			for(int j=0;j<3;++j) groups[group_count-1].tangent[j]=a0[j]; }
		rc_emit_group_t *g=&groups[slots[slot]-1]; g->area+=area;
		g->one_sided &= (t->metadata[1] & RC_SCENE_ORIENTED) != 0;
		for(int j=0;j<3;++j) {
			g->center[j]+=area*((double)t->position[0][j]+t->position[1][j]+t->position[2][j])/3.0;
			g->normal[j]+=cross[j];
		}
	}
	for(size_t i=0;i<group_count;++i) {
		rc_emit_group_t *g=&groups[i]; double distance2=0;
		for(int c=0;c<3;++c) { g->center[c]/=g->area; double d=g->center[c]-camera[c]; distance2+=d*d; }
		double normal_length=sqrt(g->normal[0]*g->normal[0]+g->normal[1]*g->normal[1]+g->normal[2]*g->normal[2]);
		if(normal_length>1e-9) for(int c=0;c<3;++c) g->normal[c]/=normal_length;
		else {g->normal[0]=g->normal[1]=0;g->normal[2]=1;}
		double projection=0; for(int c=0;c<3;++c) projection+=g->tangent[c]*g->normal[c];
		for(int c=0;c<3;++c) g->tangent[c]-=projection*g->normal[c];
		double tangent_length=sqrt(g->tangent[0]*g->tangent[0]+g->tangent[1]*g->tangent[1]+g->tangent[2]*g->tangent[2]);
		if(tangent_length<=1e-9) {
			int axis=fabs(g->normal[0])<.9?0:1; for(int c=0;c<3;++c) g->tangent[c]=(c==axis?1.0:0.0)-g->normal[axis]*g->normal[c];
			tangent_length=sqrt(g->tangent[0]*g->tangent[0]+g->tangent[1]*g->tangent[1]+g->tangent[2]*g->tangent[2]);
		}
		for(int c=0;c<3;++c) g->tangent[c]/=tangent_length;
		g->extents[0]=g->extents[2]=DBL_MAX; g->extents[1]=g->extents[3]=-DBL_MAX;
		double peak=fmax(colors[g->material][0],fmax(colors[g->material][1],colors[g->material][2]));
		g->score=g->area*peak/(1.0+distance2/(256.0*256.0));
		ranks[i]=(rc_emit_rank_t){g->score,(uint32_t)i};
	}
	/* Compute each surface's oriented bounds in its real plane. */
	for(uint32_t i=0;i<s->triangle_count;++i) {
		const rc_scene_triangle_t *t=&s->triangles[i]; uint32_t m=t->metadata[0];
		if((t->metadata[1]&RC_SCENE_SKY) || !(colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) continue;
		uint32_t surface=t->metadata[2],instance=t->metadata[3],dynamic=t->metadata[1]&RC_SCENE_DYNAMIC;
		size_t slot=(size_t)RCEmit_Hash(m,surface,instance,dynamic)&(slot_count-1);
		while(slots[slot]) { rc_emit_group_t *g=&groups[slots[slot]-1];
			if(g->material==m && g->surface==surface && g->instance==instance && g->dynamic==dynamic) {
				double v[3]={g->normal[1]*g->tangent[2]-g->normal[2]*g->tangent[1],g->normal[2]*g->tangent[0]-g->normal[0]*g->tangent[2],g->normal[0]*g->tangent[1]-g->normal[1]*g->tangent[0]};
				for(int p=0;p<3;++p) { double ucoord=0,vcoord=0;
					for(int c=0;c<3;++c) {double d=t->position[p][c]-g->center[c];ucoord+=d*g->tangent[c];vcoord+=d*v[c];}
					g->extents[0]=fmin(g->extents[0],ucoord);g->extents[1]=fmax(g->extents[1],ucoord);
					g->extents[2]=fmin(g->extents[2],vcoord);g->extents[3]=fmax(g->extents[3],vcoord);
				} break;
			} slot=(slot+1)&(slot_count-1);
		}
	}
	qsort(ranks,group_count,sizeof(*ranks),RCEmit_Order);
	size_t n=group_count<capacity ? group_count : capacity;
	for(size_t i=0;i<n;++i) {
		rc_emit_group_t *g=&groups[ranks[i].index]; g->selected=true;
		if(keys) keys[i]=(rc_emit_key_t){g->material,g->surface,g->instance,g->dynamic};
		radiance_light_t *light=&out[i]; memset(light,0,sizeof(*light));
		double peak=fmax(colors[g->material][0],fmax(colors[g->material][1],colors[g->material][2]));
		double v[3]={g->normal[1]*g->tangent[2]-g->normal[2]*g->tangent[1],g->normal[2]*g->tangent[0]-g->normal[0]*g->tangent[2],g->normal[0]*g->tangent[1]-g->normal[1]*g->tangent[0]};
		for(int c=0;c<3;++c) {
			light->position_radius[c]=(float)(g->center[c]+.5*(g->extents[0]+g->extents[1])*g->tangent[c]+.5*(g->extents[2]+g->extents[3])*v[c]);
			light->color_intensity[c]=(float)(colors[g->material][c]/peak);
			light->direction_cone[c]=(float)g->normal[c]; light->area_axis[c]=(float)g->tangent[c];
		}
		light->position_radius[3]=(float)fmin(512.0,fmax(96.0,sqrt(g->area)*16.0));
		light->color_intensity[3]=(float)fmin(4.0,peak*g->area/256.0);
		light->area_axis[3]=(float)(.5*(g->extents[1]-g->extents[0]));
		light->direction_cone[3]=(float)(.5*(g->extents[3]-g->extents[2]));
		light->metadata[0]=RADIANCE_LIGHT_EMISSIVE; light->metadata[2]=RADIANCE_LIGHT_SOURCE_TEXTURE |
			(g->one_sided ? RADIANCE_LIGHT_SOURCE_ONE_SIDED : 0);
	}
	bool flags_changed=false;
	for(uint32_t i=0;i<s->triangle_count;++i) {
		rc_scene_triangle_t *t=&s->triangles[i]; uint32_t m=t->metadata[0]; bool represented=false;
		if(!(t->metadata[1]&RC_SCENE_SKY) && (colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) {
			uint32_t surface=t->metadata[2], instance=t->metadata[3], dynamic=t->metadata[1]&RC_SCENE_DYNAMIC;
			size_t slot=(size_t)RCEmit_Hash(m,surface,instance,dynamic)&(slot_count-1);
			while(slots[slot]) { const rc_emit_group_t *g=&groups[slots[slot]-1];
				if(g->material==m && g->surface==surface && g->instance==instance && g->dynamic==dynamic) { represented=g->selected; break; }
				slot=(slot+1)&(slot_count-1);
			}
		}
		uint32_t old=t->metadata[1], replacement=(old&~RC_SCENE_EMISSIVE_PROXY)|(represented?RC_SCENE_EMISSIVE_PROXY:0);
		flags_changed|=old!=replacement; t->metadata[1]=replacement;
	}
	if(stats) *stats=(radiance_emissive_stats_t){group_count,n,group_count-n};
	if(changed) *changed=flags_changed;
	ok=true;
done:
	free(groups); free(ranks); free(slots); return ok;
}

bool Radiance_BuildEmissiveLights(rc_scene_t *s, const radiance_material_atlas_t *a,
	const float camera[3], radiance_light_t *out, size_t capacity, radiance_emissive_stats_t *stats, bool *changed)
{
	if (stats) memset(stats,0,sizeof(*stats));
	if (changed) *changed=false;
	if (!a || (a->material_count && !a->materials) || a->material_count>SIZE_MAX/(3*sizeof(double))) return false;
	double (*colors)[3]=calloc(a->material_count ? a->material_count : 1,sizeof(*colors));
	if(!colors) return false;
	bool ok=true;
	for(size_t i=0;i<a->material_count;++i) if(!RCEmit_Material(a,i,colors[i])) {ok=false;break;}
	if(ok) ok=RCEmit_BuildWithColors(s,a,(const double (*)[3])colors,camera,out,NULL,capacity,stats,changed);
	free(colors); return ok;
}

typedef struct {
	radiance_light_t *lights; rc_emit_key_t *keys; size_t count;
	uint32_t *materials; size_t material_count;
	uint32_t triangles; bool valid;
} rc_emit_part_t;
struct radiance_emissive_cache_s {
	double (*colors)[3]; uint8_t *represented; size_t material_count; uint64_t atlas_revision; bool colors_valid;
	rc_emit_part_t static_part, dynamic_part;
	rc_emit_key_t *selected; size_t selected_count; bool selected_valid;
	radiance_emissive_cache_counters_t counters;
};

static void RCEmit_ClearPart(rc_emit_part_t *part)
{ free(part->lights); free(part->keys); free(part->materials); memset(part,0,sizeof(*part)); }

/* A projectile can grow/shrink the logical material table without changing
 * any material used by the persistent scene. Compare its small dependency
 * set instead of invalidating and rescanning all persistent triangles. */
static bool RCEmit_PartColorsChanged(const rc_emit_part_t *part,
	const double (*before)[3], const uint8_t *before_represented, size_t before_count,
	const double (*after)[3], const uint8_t *after_represented, size_t after_count)
{
	if (!part->valid) return true;
	for (size_t i=0; i<part->material_count; ++i)
	{
		uint32_t material=part->materials[i];
		if (material>=before_count || material>=after_count ||
			memcmp(before[material],after[material],3*sizeof(double)) ||
			before_represented[material]!=after_represented[material]) return true;
	}
	return false;
}

radiance_emissive_cache_t *Radiance_EmissiveCacheCreate(void)
{ return calloc(1,sizeof(radiance_emissive_cache_t)); }

void Radiance_EmissiveCacheReset(radiance_emissive_cache_t *cache)
{
	if(!cache) return;
	free(cache->colors); free(cache->represented); RCEmit_ClearPart(&cache->static_part); RCEmit_ClearPart(&cache->dynamic_part); free(cache->selected);
	memset(cache,0,sizeof(*cache));
}

void Radiance_EmissiveCacheDestroy(radiance_emissive_cache_t *cache)
{ if(cache) { Radiance_EmissiveCacheReset(cache); free(cache); } }

void Radiance_EmissiveCacheCounters(const radiance_emissive_cache_t *cache, radiance_emissive_cache_counters_t *out)
{ if(out) *out=cache ? cache->counters : (radiance_emissive_cache_counters_t){0}; }

static bool RCEmit_BuildPart(rc_emit_part_t *part, const rc_scene_triangle_t *triangles, uint32_t count,
	const radiance_material_atlas_t *atlas, const double (*colors)[3], const float camera[3])
{
	uint32_t eligible=0;size_t material_count=0;
	uint8_t *used=calloc(atlas->material_count ? atlas->material_count : 1,1);
	if (!used) return false;
	for(uint32_t i=0;i<count;++i) {
		uint32_t m=triangles[i].metadata[0]; if(m>=atlas->material_count) {free(used);return false;}
		if (!used[m]) {used[m]=1;++material_count;}
		if(!(triangles[i].metadata[1]&RC_SCENE_SKY) &&
			(colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) ++eligible;
	}
	if ((uint64_t)eligible*sizeof(*triangles)>SIZE_MAX || (uint64_t)eligible*sizeof(radiance_light_t)>SIZE_MAX ||
		(uint64_t)eligible*sizeof(rc_emit_key_t)>SIZE_MAX || material_count>SIZE_MAX/sizeof(uint32_t)) {free(used);return false;}
	uint32_t *materials=material_count ? malloc(material_count*sizeof(*materials)) : NULL;
	if (material_count && !materials) {free(used);return false;}
	for (size_t i=0,at=0; i<atlas->material_count; ++i) if (used[i]) materials[at++]=(uint32_t)i;
	free(used);
	rc_scene_triangle_t *copy=eligible ? malloc((size_t)eligible*sizeof(*copy)) : NULL;
	radiance_light_t *lights=eligible ? malloc((size_t)eligible*sizeof(*lights)) : NULL;
	rc_emit_key_t *keys=eligible ? malloc((size_t)eligible*sizeof(*keys)) : NULL;
	if(eligible && (!copy||!lights||!keys)) { free(copy);free(lights);free(keys);free(materials);return false; }
	uint32_t at=0;
	for(uint32_t i=0;i<count;++i) {
		uint32_t m=triangles[i].metadata[0];
		if(!(triangles[i].metadata[1]&RC_SCENE_SKY) &&
			(colors[m][0]>0 || colors[m][1]>0 || colors[m][2]>0)) copy[at++]=triangles[i];
	}
	rc_scene_t view={.triangles=copy,.triangle_count=eligible}; radiance_emissive_stats_t stats;
	bool ok=RCEmit_BuildWithColors(&view,atlas,colors,camera,lights,keys,eligible,&stats,NULL);
	free(copy);
	if(!ok) {free(lights);free(keys);free(materials);return false;}
	RCEmit_ClearPart(part); *part=(rc_emit_part_t){.lights=lights,.keys=keys,.count=stats.emitted_lights,
		.materials=materials,.material_count=material_count,.triangles=count,.valid=true};
	return true;
}

static bool RCEmit_KeyEqual(const rc_emit_key_t *a,const rc_emit_key_t *b)
{ return a->material==b->material && a->surface==b->surface && a->instance==b->instance && a->dynamic==b->dynamic; }
static bool RCEmit_KeyLess(const rc_emit_key_t *a,const rc_emit_key_t *b)
{
	if(a->dynamic!=b->dynamic) return a->dynamic<b->dynamic;
	if(a->instance!=b->instance) return a->instance<b->instance;
	if(a->surface!=b->surface) return a->surface<b->surface;
	return a->material<b->material;
}

static bool RCEmit_Selected(const rc_emit_key_t *keys,size_t count,const rc_emit_key_t *wanted)
{
	size_t lo=0,hi=count;
	while (lo<hi) {size_t at=lo+(hi-lo)/2;if(RCEmit_KeyLess(&keys[at],wanted))lo=at+1;else hi=at;}
	return lo<count && RCEmit_KeyEqual(&keys[lo],wanted);
}

static bool RCEmit_PartSelectionChanged(const rc_emit_part_t *part,
	const rc_emit_key_t *before,size_t before_count,const rc_emit_key_t *after,size_t after_count)
{
	for(size_t i=0;i<part->count;++i)
		if(RCEmit_Selected(before,before_count,&part->keys[i])!=RCEmit_Selected(after,after_count,&part->keys[i])) return true;
	return false;
}

bool Radiance_BuildEmissiveLightsCachedFiltered(radiance_emissive_cache_t *cache, rc_scene_t *scene,
	uint32_t static_count, const radiance_material_atlas_t *atlas, uint64_t revision,
	bool scene_rebuilt, bool dynamic_changed, const uint8_t *represented_materials, const float camera[3], radiance_light_t *out,
	size_t capacity, radiance_emissive_stats_t *stats, bool *changed)
{
	if(stats) memset(stats,0,sizeof(*stats));
	if(changed) *changed=false;
	if(!cache || !scene || !atlas || !camera || static_count>scene->triangle_count ||
		(scene->triangle_count && !scene->triangles) || (atlas->material_count && !atlas->materials) ||
		(capacity && !out) || atlas->material_count>UINT32_MAX || atlas->material_count>SIZE_MAX/(3*sizeof(double))) return false;
	for(int c=0;c<3;++c) if(!isfinite(camera[c])) return false;
	bool color_changed=!cache->colors_valid || revision!=cache->atlas_revision || atlas->material_count!=cache->material_count;
	bool part_colors_changed[2]={false,false};
	for(size_t i=0;!color_changed && i<atlas->material_count;++i)
		if(cache->represented[i] != (represented_materials && represented_materials[i] ? 1u : 0u)) color_changed=true;
	if(color_changed) {
		double (*colors)[3]=calloc(atlas->material_count ? atlas->material_count : 1,sizeof(*colors)); if(!colors) return false;
		uint8_t *represented=calloc(atlas->material_count ? atlas->material_count : 1,1); if(!represented) {free(colors);return false;}
		for(size_t i=0;i<atlas->material_count;++i) {
			represented[i]=represented_materials && represented_materials[i] ? 1u : 0u;
			if(!RCEmit_Material(atlas,i,colors[i])) {free(colors);free(represented);return false;}
			if(represented[i]) memset(colors[i],0,sizeof(colors[i]));
		}
		part_colors_changed[0]=!cache->colors_valid || RCEmit_PartColorsChanged(&cache->static_part,
			(const double (*)[3])cache->colors,cache->represented,cache->material_count,
			(const double (*)[3])colors,represented,atlas->material_count);
		part_colors_changed[1]=!cache->colors_valid || RCEmit_PartColorsChanged(&cache->dynamic_part,
			(const double (*)[3])cache->colors,cache->represented,cache->material_count,
			(const double (*)[3])colors,represented,atlas->material_count);
		free(cache->colors); free(cache->represented); cache->colors=colors; cache->represented=represented;
		cache->colors_valid=true; cache->material_count=atlas->material_count; cache->atlas_revision=revision;
		if(part_colors_changed[0])RCEmit_ClearPart(&cache->static_part);
		if(part_colors_changed[1])RCEmit_ClearPart(&cache->dynamic_part);
		++cache->counters.material_rebuilds;
	}
	bool static_rebuilt=false,dynamic_rebuilt=false;
	if(!cache->static_part.valid || cache->static_part.triangles!=static_count) {
		if(!RCEmit_BuildPart(&cache->static_part,scene->triangles,static_count,atlas,(const double (*)[3])cache->colors,camera)) return false;
		++cache->counters.static_rebuilds;static_rebuilt=true;
	}
	uint32_t dynamic_count=scene->triangle_count-static_count;
	if(!cache->dynamic_part.valid || cache->dynamic_part.triangles!=dynamic_count || scene_rebuilt || dynamic_changed) {
		const rc_scene_triangle_t *dynamic_triangles=dynamic_count ? scene->triangles+static_count : NULL;
		if(!RCEmit_BuildPart(&cache->dynamic_part,dynamic_triangles,dynamic_count,atlas,(const double (*)[3])cache->colors,camera)) return false;
		++cache->counters.dynamic_rebuilds;dynamic_rebuilt=true;
	}
	if(cache->static_part.count>SIZE_MAX-cache->dynamic_part.count) return false;
	size_t total=cache->static_part.count+cache->dynamic_part.count, n=total<capacity?total:capacity;
	if(total>UINT32_MAX || total>SIZE_MAX/sizeof(rc_emit_rank_t) || n>SIZE_MAX/sizeof(rc_emit_key_t) || n>SIZE_MAX/sizeof(radiance_light_t)) return false;
	rc_emit_rank_t *ranks=malloc((total?total:1)*sizeof(*ranks));
	rc_emit_key_t *keys=malloc((n?n:1)*sizeof(*keys)); radiance_light_t *lights=malloc((n?n:1)*sizeof(*lights));
	if(!ranks||!keys||!lights) {free(ranks);free(keys);free(lights);return false;}
	for(size_t i=0;i<total;++i) {
		const radiance_light_t *l=i<cache->static_part.count?&cache->static_part.lights[i]:&cache->dynamic_part.lights[i-cache->static_part.count];
		double u[3]={l->area_axis[0],l->area_axis[1],l->area_axis[2]}, normal[3]={l->direction_cone[0],l->direction_cone[1],l->direction_cone[2]};
		double v[3]={normal[1]*u[2]-normal[2]*u[1],normal[2]*u[0]-normal[0]*u[2],normal[0]*u[1]-normal[1]*u[0]};
		double rel[3],du=0,dv=0; for(int c=0;c<3;++c) {rel[c]=(double)camera[c]-l->position_radius[c];du+=rel[c]*u[c];dv+=rel[c]*v[c];}
		du=fmax(-l->area_axis[3],fmin(l->area_axis[3],du)); dv=fmax(-l->direction_cone[3],fmin(l->direction_cone[3],dv));
		double d2=0; for(int c=0;c<3;++c) {double d=rel[c]-du*u[c]-dv*v[c];d2+=d*d;}
		ranks[i]=(rc_emit_rank_t){l->color_intensity[3]/(1.0+d2/(256.0*256.0)),(uint32_t)i};
	}
	if(total>capacity) qsort(ranks,total,sizeof(*ranks),RCEmit_Order);
	for(size_t i=0;i<n;++i) {
		uint32_t index=ranks[i].index; const rc_emit_part_t *part=index<cache->static_part.count?&cache->static_part:&cache->dynamic_part;
		if(part==&cache->dynamic_part) index-=(uint32_t)cache->static_part.count;
		keys[i]=part->keys[index]; lights[i]=part->lights[index];
	}
	/* Stable source ordering prevents unrelated light-grid/cascade refreshes as
	 * camera distances reorder an unchanged selection. */
	for(size_t i=1;i<n;++i) {
		rc_emit_key_t key=keys[i]; radiance_light_t light=lights[i]; size_t j=i;
		while(j && RCEmit_KeyLess(&key,&keys[j-1])) {keys[j]=keys[j-1];lights[j]=lights[j-1];--j;}
		keys[j]=key; lights[j]=light;
	}
	bool selection_changed=!cache->selected_valid || cache->selected_count!=n;
	for(size_t i=0;!selection_changed && i<n;++i) if(!RCEmit_KeyEqual(&cache->selected[i],&keys[i])) selection_changed=true;
	if(n>SIZE_MAX/2){free(ranks);free(keys);free(lights);return false;}
	size_t table_size=1; while(table_size<n*2) {if(table_size>SIZE_MAX/2){free(ranks);free(keys);free(lights);return false;} table_size*=2;}
	uint32_t *table=calloc(table_size,sizeof(*table));
	if(!table) {free(ranks);free(keys);free(lights);return false;}
	for(size_t i=0;i<n;++i) {
		size_t slot=(size_t)RCEmit_Hash(keys[i].material,keys[i].surface,keys[i].instance,keys[i].dynamic)&(table_size-1);
		while(table[slot]) slot=(slot+1)&(table_size-1);
		table[slot]=(uint32_t)i+1;
	}
	bool flag_changed=false;
	for(int part=0;part<2;++part) {
		const rc_emit_part_t *cached=part ? &cache->dynamic_part : &cache->static_part;
		bool selected_changed=selection_changed && RCEmit_PartSelectionChanged(cached,cache->selected,
			cache->selected_count,keys,n);
		bool scan=selected_changed || part_colors_changed[part] || scene_rebuilt ||
			(part ? dynamic_changed || dynamic_rebuilt : static_rebuilt);
		if(!scan) continue;
		uint32_t begin=part ? static_count : 0, end=part ? scene->triangle_count : static_count;
		if(!part) cache->counters.static_triangle_scans+=end-begin;
		for(uint32_t i=begin;i<end;++i) {
			rc_scene_triangle_t *t=&scene->triangles[i]; rc_emit_key_t key={t->metadata[0],t->metadata[2],t->metadata[3],t->metadata[1]&RC_SCENE_DYNAMIC};
			size_t slot=(size_t)RCEmit_Hash(key.material,key.surface,key.instance,key.dynamic)&(table_size-1);
			bool represented=key.material<cache->material_count && cache->represented[key.material] && !(t->metadata[1]&RC_SCENE_SKY);
			if(!(t->metadata[1] & RC_SCENE_SKY))
				while(table[slot]) {if(RCEmit_KeyEqual(&keys[table[slot]-1],&key)){represented=true;break;}slot=(slot+1)&(table_size-1);}
			uint32_t old=t->metadata[1], value=(old&~RC_SCENE_EMISSIVE_PROXY)|(represented?RC_SCENE_EMISSIVE_PROXY:0);
			flag_changed|=old!=value; t->metadata[1]=value;
		}
	}
	free(cache->selected); cache->selected=keys; cache->selected_count=n; cache->selected_valid=true;
	if(n) memcpy(out,lights,n*sizeof(*out));
	if(stats) *stats=(radiance_emissive_stats_t){total,n,total-n};
	if(changed) *changed=flag_changed;
	free(table);free(ranks);free(lights);return true;
}

bool Radiance_BuildEmissiveLightsCached(radiance_emissive_cache_t *cache, rc_scene_t *scene,
	uint32_t static_count, const radiance_material_atlas_t *atlas, uint64_t revision,
	bool scene_rebuilt, bool dynamic_changed, const float camera[3], radiance_light_t *out,
	size_t capacity, radiance_emissive_stats_t *stats, bool *changed)
{
	return Radiance_BuildEmissiveLightsCachedFiltered(cache,scene,static_count,atlas,revision,scene_rebuilt,
		dynamic_changed,NULL,camera,out,capacity,stats,changed);
}
