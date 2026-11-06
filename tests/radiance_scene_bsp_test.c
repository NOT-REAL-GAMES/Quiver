#include "../Quake/quakedef.h"
#include "../Quake/radiance_scene_bsp.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static uint32_t material_id (const texture_t *texture, void *user)
{
	return texture ? *(const uint32_t *)user : UINT32_MAX;
}

int main (void)
{
	rc_scene_t scene; RCScene_Init (&scene);
	texture_t texture = {0}; texture.type = TEXTYPE_CUTOUT; texture.fullbright = (struct gltexture_s *)&texture;
	mtexinfo_t texinfo = {0}; texinfo.texture = &texture;
	glpoly_t poly = {0}; poly.numverts = 4;
	const float vertices[4][5] = {{0,0,0,0,0},{1,0,0,1,0},{1,1,0,1,1},{0,1,0,0,1}};
	for (int i=0;i<4;++i) for (int j=0;j<5;++j) poly.verts[i][j]=vertices[i][j];
	msurface_t surface = {0}; surface.texinfo=&texinfo; surface.polys=&poly;
	qmodel_t model = {0}; model.type=mod_brush; model.surfaces=&surface; model.numsurfaces=1; model.nummodelsurfaces=1;
	float matrix[16] = {1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1}; uint32_t mat = 12;
	assert (RCScene_AppendBrush (&scene,&model,matrix,7,true,material_id,&mat));
	assert (scene.triangle_count==2 && RCScene_Build (&scene));
	float origin[3]={10.75f,20.25f,31}, direction[3]={0,0,-1}; rc_scene_hit_t hit;
	assert (RCScene_TraceReference (&scene,origin,direction,0,10,NULL,NULL,&hit));
	assert (fabsf(hit.distance-1)<1e-5f && fabsf(hit.uv[0]-.75f)<1e-5f && fabsf(hit.uv[1]-.25f)<1e-5f);
	assert (hit.material==12 && hit.surface==0 && hit.instance==7);
	assert ((hit.flags & (RC_SCENE_CUTOUT|RC_SCENE_EMISSIVE|RC_SCENE_DYNAMIC)) == (RC_SCENE_CUTOUT|RC_SCENE_EMISSIVE|RC_SCENE_DYNAMIC));
	model.firstmodelsurface=1;
	assert (!RCScene_AppendBrush (&scene,&model,NULL,0,false,material_id,&mat));
	assert (scene.triangle_count==2);
	RCScene_Clear (&scene);
	/* Plane side, not surfedge winding, determines the visible emission side.
	 * UVs stay attached to their vertices when the triangle is reversed. */
	model.firstmodelsurface=0;
	mplane_t plane = {0}; plane.normal[2]=1; surface.plane=&plane;
	for (int back=0; back<2; ++back)
	for (int reflected=0; reflected<2; ++reflected)
	for (int rotated=0; rotated<2; ++rotated)
	{
		surface.flags = back ? SURF_PLANEBACK : 0;
		float transform[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
		if (reflected) transform[0]=-1;
		if (rotated) { transform[1]=transform[0]; transform[0]=0; transform[4]=-1; transform[5]=0; }
		assert (RCScene_AppendBrush (&scene,&model,transform,0,false,material_id,&mat));
		const rc_scene_triangle_t *tri=&scene.triangles[0];
		float abx=tri->position[1][0]-tri->position[0][0], aby=tri->position[1][1]-tri->position[0][1];
		float acx=tri->position[2][0]-tri->position[0][0], acy=tri->position[2][1]-tri->position[0][1];
		assert ((tri->metadata[1]&RC_SCENE_ORIENTED) && (abx*acy-aby*acx)*(back ? -1 : 1)>0);
		for (int p=0;p<3;++p)
		{
			float u=p==0 ? tri->uv01[0] : p==1 ? tri->uv01[2] : tri->uv2[0];
			float v=p==0 ? tri->uv01[1] : p==1 ? tri->uv01[3] : tri->uv2[1];
			float x=rotated ? tri->position[p][1] : tri->position[p][0];
			float y=rotated ? -tri->position[p][0] : tri->position[p][1];
			assert (fabsf(u-(reflected ? -x : x))<1e-5f && fabsf(v-y)<1e-5f);
		}
		RCScene_Clear (&scene);
	}
	puts ("PASS: transformed brush triangulation, UVs and material flags");
	return 0;
}
