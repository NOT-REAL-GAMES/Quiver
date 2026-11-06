#include "../Quake/radiance_scene.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static rc_scene_triangle_t triangle (float x, float y, float z, uint32_t material)
{
	rc_scene_triangle_t t = {0};
	t.position[0][0]=x; t.position[0][1]=y; t.position[0][2]=z;
	t.position[1][0]=x+1; t.position[1][1]=y; t.position[1][2]=z;
	t.position[2][0]=x; t.position[2][1]=y+1; t.position[2][2]=z;
	t.uv01[2]=1; t.uv2[1]=1; t.metadata[0]=material; t.metadata[2]=material+10;
	return t;
}
static bool skip_first (const rc_scene_hit_t *hit, void *user) { (void)user; return hit->material != 3; }

static void test_composed_forest (void)
{
	rc_scene_t parts[2]={{0}}, combined={0}, empty={0};
	for(unsigned part=0;part<2;++part)
	{
		for(unsigned i=0;i<19+part;++i)
		{
			rc_scene_triangle_t t=triangle(i*2,part*4,part*3,i+part*100);
			t.metadata[1]=RC_SCENE_DYNAMIC|RC_SCENE_CUTOUT;t.metadata[3]=part+5;
			assert(RCScene_AppendTriangle(&parts[part],&t));
		}
		assert(RCScene_Build(&parts[part]));
	}
	assert(RCScene_Compose(&combined,&parts[0],&parts[1]));
	assert(combined.triangle_count==39 && combined.index_count==39);
	assert(combined.node_count==1+parts[0].node_count+parts[1].node_count);
	assert(!memcmp(combined.triangles,parts[0].triangles,19*sizeof(*combined.triangles)));
	assert(!memcmp(combined.triangles+19,parts[1].triangles,20*sizeof(*combined.triangles)));
	assert(RCScene_Refit(&combined)); /* validates all relocated links/leaf indices */
	const rc_scene_triangle_t *allocation=combined.triangles;
	combined.triangles[0].metadata[1]|=RC_SCENE_EMISSIVE_PROXY;
	parts[1].triangles[0].position[0][2]+=1;assert(RCScene_Refit(&parts[1]));
	assert(RCScene_ReplaceSuffix(&combined,&parts[0],&parts[1]) && combined.triangles==allocation);
	assert(combined.triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
	assert(combined.triangles[19].position[0][2]==4);
	parts[1].triangles[0].position[0][2]-=1;assert(RCScene_Refit(&parts[1]));
	assert(RCScene_ReplaceSuffix(&combined,&parts[0],&parts[1]));
	assert(!RCScene_ReplaceSuffix(&combined,&empty,&parts[1]) && combined.triangles==allocation);
	for(unsigned part=0;part<2;++part) for(unsigned i=0;i<19+part;++i)
	{
		float origin[3]={i*2+.25f,part*4+.25f,10},direction[3]={0,0,-1};
		rc_scene_hit_t hit;
		assert(RCScene_TraceReference(&combined,origin,direction,0,20,NULL,NULL,&hit));
		assert(hit.material==i+part*100 && hit.instance==part+5 && (hit.flags&RC_SCENE_CUTOUT));
		assert(fabsf(hit.distance-(10-part*3))<1e-5f && fabsf(hit.uv[0]-.25f)<1e-5f);
	}
	rc_scene_triangle_t *before=combined.triangles;
	uint32_t escape=parts[0].nodes[0].metadata[2];parts[0].nodes[0].metadata[2]=0;
	assert(!RCScene_Compose(&combined,&parts[0],&parts[1]) && combined.triangles==before);
	parts[0].nodes[0].metadata[2]=escape;
	assert(RCScene_Compose(&combined,&empty,&parts[1]) && combined.triangle_count==20);
	assert(RCScene_Compose(&combined,&parts[0],&empty) && combined.triangle_count==19);
	assert(RCScene_Compose(&combined,&empty,&empty) && !combined.triangle_count && !combined.node_count);
	RCScene_Clear(&parts[0]);RCScene_Clear(&parts[1]);RCScene_Clear(&combined);
}

int main (void)
{
	test_composed_forest ();
	rc_scene_t scene; RCScene_Init (&scene); assert (RCScene_Build (&scene));
	rc_scene_hit_t hit;
	const float origin[3] = {.25f,.25f,10}, direction[3] = {0,0,-1};
	assert (!RCScene_TraceReference (&scene,origin,direction,0,100,NULL,NULL,&hit));
	rc_scene_triangle_t a = triangle (0,0,8,3), b = triangle (0,0,2,7);
	assert (RCScene_AppendTriangle (&scene,&a)); assert (RCScene_AppendTriangle (&scene,&b));
	assert (RCScene_Build (&scene));
	assert (RCScene_TraceReference (&scene,origin,direction,0,100,NULL,NULL,&hit));
	assert (hit.material==3 && fabsf(hit.distance-2)<1e-5f && fabsf(hit.uv[0]-.25f)<1e-5f && fabsf(hit.uv[1]-.25f)<1e-5f);
	assert (RCScene_TraceReference (&scene,origin,direction,0,100,skip_first,NULL,&hit));
	assert (hit.material==7 && hit.surface==17 && fabsf(hit.distance-8)<1e-5f);
	float query_mins[3]={0,0,8}, query_maxs[3]={1,1,8};
	assert (RCScene_AnyGeometryAABB (&scene,query_mins,query_maxs));
	query_mins[2]=9; query_maxs[2]=10;
	assert (!RCScene_AnyGeometryAABB (&scene,query_mins,query_maxs));
	assert (!RCScene_TraceReference (&scene,origin,direction,0,1,NULL,NULL,&hit));
	const float miss[3] = {2,2,10};
	assert (!RCScene_TraceReference (&scene,miss,direction,0,100,NULL,NULL,&hit));
	/* Exercise balanced construction with identical centroids and deep enough
	 * trees for threaded escape links to skip nested subtrees. */
	for (uint32_t i=0;i<1024;++i)
	{
		rc_scene_triangle_t t=triangle ((float)(i%32)*2,(float)(i/32)*2,0,i+100);
		assert (RCScene_AppendTriangle (&scene,&t));
	}
	for (uint32_t i=0;i<32;++i) assert (RCScene_AppendTriangle (&scene,&a));
	assert (RCScene_Build (&scene));
	assert (scene.index_count==scene.triangle_count && scene.node_count>1);
	for (uint32_t i=0;i<1024;++i)
	{
		float p[3]={(float)(i%32)*2+.25f,(float)(i/32)*2+.25f,1};
		assert (RCScene_TraceReference (&scene,p,direction,0,2,NULL,NULL,&hit));
		assert (hit.material==i+100 && fabsf(hit.distance-1)<1e-5f);
	}
	rc_scene_triangle_t invalid=a; invalid.position[1][0]=NAN;
	assert (!RCScene_AppendTriangle (&scene,&invalid));
	assert (RCScene_TraceReference (&scene,origin,direction,0,100,NULL,NULL,&hit));
	RCScene_Clear (&scene); assert (!scene.triangles && !scene.indices && !scene.nodes);
	puts ("PASS: scene build, threaded BVH, closest hits, UVs, alpha rejection, invalid geometry");
	return 0;
}
