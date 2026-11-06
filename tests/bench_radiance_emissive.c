/* Isolated CPU cost of a mostly static world plus changing dynamic geometry.
 * Build: cc -O3 -std=gnu11 -IQuake tests/bench_radiance_emissive.c Quake/radiance_emissive.c -lm -o /tmp/bench-radiance-emissive */
#include "radiance_emissive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1000000.0; }
static int cmp(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y); }
int main(void)
{
	const uint32_t static_count=100000,dynamic_count=64,total=static_count+dynamic_count;
	rc_scene_triangle_t *t=calloc(total,sizeof(*t)); assert(t);
	for(uint32_t i=0;i<total;++i) {
		float x=(float)(i%512)*8,y=(float)(i/512)*8;
		t[i].position[0][0]=x;t[i].position[0][1]=y;
		t[i].position[1][0]=x+4;t[i].position[1][1]=y;
		t[i].position[2][0]=x;t[i].position[2][1]=y+4;
		t[i].metadata[0]=(i>=static_count || i%1000==0)?1:0;
		t[i].metadata[2]=i; if(i>=static_count){t[i].metadata[1]=RC_SCENE_DYNAMIC;t[i].metadata[3]=i;}
	}
	rc_scene_t scene={.triangles=t,.triangle_count=total};
	uint8_t pixel[4]={255,255,255,255}; radiance_material_t materials[2]={0};
	materials[1].layers_flags[2]=RADIANCE_MATERIAL_EMISSION_TEXTURE;
	materials[1].emission[0]=materials[1].emission[1]=materials[1].emission[2]=materials[1].emission[3]=1;
	materials[1].emission_rect[2]=materials[1].emission_rect[3]=1;
	radiance_material_atlas_t atlas={.materials=materials,.material_count=2,.rgba=pixel,.bytes=4,.width=1,.height=1,.layers=1};
	radiance_emissive_cache_t *cache=Radiance_EmissiveCacheCreate(); assert(cache);
	float camera[3]={0}; radiance_light_t lights[256]; radiance_emissive_stats_t stats; bool changed;
	double start=ms(); assert(Radiance_BuildEmissiveLightsCached(cache,&scene,static_count,&atlas,1,true,true,camera,lights,256,&stats,&changed));
	double initial=ms()-start, samples[200];
	for(int frame=0;frame<200;++frame) {
		camera[0]+=.25f; for(uint32_t j=static_count;j<total;++j) t[j].position[0][2]+=.001f;
		start=ms(); assert(Radiance_BuildEmissiveLightsCached(cache,&scene,static_count,&atlas,1,false,true,camera,lights,256,&stats,&changed)); samples[frame]=ms()-start;
	}
	qsort(samples,200,sizeof(*samples),cmp); radiance_emissive_cache_counters_t counters; Radiance_EmissiveCacheCounters(cache,&counters);
	assert(counters.static_rebuilds==1 && counters.material_rebuilds==1 && counters.static_triangle_scans==static_count);
	printf("static=%u dynamic=%u emitters=%zu initial=%.3f ms dynamic-median=%.3f ms p95=%.3f ms static-scans=%llu\n",
		static_count,dynamic_count,stats.emitted_lights,initial,(samples[99]+samples[100])*.5,samples[189],(unsigned long long)counters.static_triangle_scans);
	Radiance_EmissiveCacheDestroy(cache); free(t); return 0;
}
