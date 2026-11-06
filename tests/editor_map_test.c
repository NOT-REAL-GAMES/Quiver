/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_map.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bounds(const editor_map_brush_t *b, const double *lo, const double *hi)
{
    double a[3], z[3];
    assert(EditorMap_BrushBounds(b, a, z));
    for (int i = 0; i < 3; ++i) {
        assert(fabs(a[i] - lo[i]) < 1e-7);
        assert(fabs(z[i] - hi[i]) < 1e-7);
    }
}

static void point_entities(void)
{
    editor_map_t m={0},loaded={0};double p[3]={24,-32,96},delta[3]={16,16,0},out[3];size_t light,duplicate;
    assert(EditorMap_Init(&m));assert(EditorMap_AddLight(&m,p,&light));
    assert(!strcmp(EditorMap_GetProperty(&m.entities[light],"classname"),"light"));
    assert(!strcmp(EditorMap_GetProperty(&m.entities[light],"light"),"300"));
    assert(EditorMap_SetProperty(&m.entities[light],"targetname","switch_a"));
    assert(EditorMap_SetProperty(&m.entities[light],"style","2"));
    assert(EditorMap_SetProperty(&m.entities[light],"_color","0.2 0.4 0.8"));
    assert(EditorMap_DuplicatePoint(&m,light,delta,&duplicate));
    assert(duplicate==light+1&&EditorMap_GetPointOrigin(&m,duplicate,out));
    assert(out[0]==40&&out[1]==-16&&out[2]==96);
    for(size_t i=0;i<m.entities[light].num_properties;++i){const editor_map_property_t *prop=&m.entities[light].properties[i];
        if(strcmp(prop->key,"origin"))assert(!strcmp(EditorMap_GetProperty(&m.entities[duplicate],prop->key),prop->value));}
    p[0]=80;assert(EditorMap_SetPointOrigin(&m,duplicate,p));
    assert(EditorMap_GetPointOrigin(&m,light,out)&&out[0]==24);
    assert(!EditorMap_DuplicatePoint(&m,0,delta,NULL));
    double invalid[3]={NAN,0,0};size_t count=m.num_entities;
    assert(!EditorMap_DuplicatePoint(&m,light,invalid,NULL)&&m.num_entities==count);
    assert(!EditorMap_SetPointOrigin(&m,duplicate,invalid));
    const double beyond[3]={EDITOR_MAP_POINT_COORD_LIMIT,0,0};
    assert(!EditorMap_DuplicatePoint(&m,light,beyond,NULL)&&m.num_entities==count);
    char *text=NULL,error[128];size_t length;
    assert(EditorMap_Serialize(&m,&text,&length));assert(EditorMap_Parse(&loaded,text,length,error,sizeof error));free(text);
    assert(EditorMap_GetPointOrigin(&loaded,duplicate,out)&&out[0]==80&&out[1]==-32&&out[2]==96);
    assert(!strcmp(EditorMap_GetProperty(&loaded.entities[duplicate],"targetname"),"switch_a"));
    assert(EditorMap_SetProperty(&loaded.entities[duplicate],"origin","1 nope 3"));
    assert(!EditorMap_GetPointOrigin(&loaded,duplicate,out));
    EditorMap_Free(&loaded);EditorMap_Free(&m);
}

static void light_colors(void)
{
    editor_map_t map={0},undo={0},redo={0},loaded={0};
    const double origin[3]={16,32,48},offset[3]={32,0,0},rgb[3]={1.0/255,64.0/255,0};
    double got[3];size_t light,copy;
    assert(EditorMap_Init(&map)&&EditorMap_AddLight(&map,origin,&light));
    assert(EditorMap_GetLightColor(&map,light,got)&&got[0]==1&&got[1]==1&&got[2]==1);
    assert(!EditorMap_GetLightColor(&map,0,got)&&!EditorMap_SetLightColor(&map,0,rgb));
    assert(EditorMap_SetProperty(&map.entities[light],"targetname","keep_switch"));
    assert(EditorMap_SetProperty(&map.entities[light],"style","3"));
    assert(EditorMap_SetProperty(&map.entities[light],"_color","0.1 0.2 0.3"));
    assert(EditorMap_GetLightColor(&map,light,got)&&fabs(got[1]-.2)<1e-15);
    assert(EditorMap_SetProperty(&map.entities[light],"color","255 128 0"));
    assert(EditorMap_GetLightColor(&map,light,got)&&got[0]==1&&fabs(got[1]-128.0/255)<1e-15&&got[2]==0);
    assert(EditorMap_Clone(&undo,&map));
    assert(EditorMap_SetLightColor(&map,light,rgb));
    assert(!EditorMap_GetProperty(&map.entities[light],"color"));
    assert(!strcmp(EditorMap_GetProperty(&map.entities[light],"targetname"),"keep_switch"));
    assert(!strcmp(EditorMap_GetProperty(&map.entities[light],"style"),"3"));
    assert(!strcmp(EditorMap_GetProperty(&map.entities[light],"light"),"300"));
    assert(EditorMap_GetLightColor(&map,light,got));
    for(int a=0;a<3;++a)assert(got[a]==rgb[a]);
    assert(EditorMap_Clone(&redo,&map)); /* exact before/after snapshots used by history */
    assert(EditorMap_Clone(&map,&undo));
    assert(!strcmp(EditorMap_GetProperty(&map.entities[light],"color"),"255 128 0"));
    assert(EditorMap_Clone(&map,&redo)&&!EditorMap_GetProperty(&map.entities[light],"color"));
    double bad[3]={NAN,0,0};assert(!EditorMap_SetLightColor(&map,light,bad));
    bad[0]=INFINITY;assert(!EditorMap_SetLightColor(&map,light,bad));
    bad[0]=-.01;assert(!EditorMap_SetLightColor(&map,light,bad));
    bad[0]=1.01;assert(!EditorMap_SetLightColor(&map,light,bad));
    assert(EditorMap_GetLightColor(&map,light,got)&&!memcmp(got,rgb,sizeof(got)));
    assert(EditorMap_DuplicatePoint(&map,light,offset,&copy));
    assert(EditorMap_GetLightColor(&map,copy,got)&&!memcmp(got,rgb,sizeof(got)));
    char *text=NULL,error[128];size_t length;
    assert(EditorMap_Serialize(&map,&text,&length)&&EditorMap_Parse(&loaded,text,length,error,sizeof(error)));free(text);
    assert(EditorMap_GetLightColor(&loaded,copy,got)&&!memcmp(got,rgb,sizeof(got)));
    const char *invalid[]={"nan 0 1","1 2","1 2 3 junk","inf 2 3"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i){
        assert(EditorMap_SetProperty(&loaded.entities[copy],"_color",invalid[i]));
        assert(EditorMap_GetLightColor(&loaded,copy,got)&&got[0]==1&&got[1]==1&&got[2]==1);
        assert(!strcmp(EditorMap_GetProperty(&loaded.entities[copy],"_color"),invalid[i]));}
    /* RGB-valued light imports retain their effective color until an edit. */
    size_t embedded;assert(EditorMap_AddEntity(&loaded,"light",&embedded));
    assert(EditorMap_SetProperty(&loaded.entities[embedded],"light","255 0 128 600"));
    assert(EditorMap_GetLightColor(&loaded,embedded,got)&&got[0]==1&&got[1]==0&&fabs(got[2]-128.0/255)<1e-15);
    assert(EditorMap_SetLightColor(&loaded,embedded,rgb));
    assert(!strcmp(EditorMap_GetProperty(&loaded.entities[embedded],"light"),"255 0 128 600"));
    assert(EditorMap_GetLightColor(&loaded,embedded,got)&&!memcmp(got,rgb,sizeof(got)));
    const double black[3]={0,0,0};assert(EditorMap_SetLightColor(&loaded,embedded,black));
    assert(!strcmp(EditorMap_GetProperty(&loaded.entities[embedded],"_color"),"0 0 0"));
    assert(EditorMap_Serialize(&loaded,&text,&length)&&EditorMap_Parse(&map,text,length,error,sizeof(error)));free(text);
    assert(EditorMap_GetLightColor(&map,embedded,got)&&!memcmp(got,black,sizeof(got)));
    EditorMap_Free(&map);EditorMap_Free(&undo);EditorMap_Free(&redo);EditorMap_Free(&loaded);
}

static void model_entities(void)
{
    editor_map_t map={0},saved={0},loaded={0};editor_map_model_t model,check;
    const double origin[3]={24,-32,96},offset[3]={32,0,16};size_t index,duplicate;
    assert(EditorMap_Init(&map));
    assert(EditorMap_ValidModelPath("progs/sponza.md5mesh")&&EditorMap_ValidModelPath("models/props/pillar.MD3"));
    const char *invalid[]={"","/tmp/a.mdl","../a.mdl","progs/../a.mdl","progs//a.mdl","progs/a.md5anim","progs/a.bsp","progs/a.mdl;quit"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i)assert(!EditorMap_ValidModelPath(invalid[i]));
    assert(EditorMap_AddModel(&map,"progs/sponza.md5mesh",origin,&index));
    assert(EditorMap_GetModel(&map,index,&model)&&model.modelscale==1&&model.skin==0&&model.collision);
    assert(!memcmp(model.origin,origin,sizeof(origin))&&model.angles[0]==0&&model.angles[1]==0);
    assert(EditorMap_Clone(&saved,&map));
    model.angles[0]=-15;model.angles[1]=90;model.angles[2]=12;model.modelscale=1.25;model.skin=2;model.collision=false;
    assert(EditorMap_SetModel(&map,index,&model));
    assert(EditorMap_GetModel(&saved,index,&check)&&check.modelscale==1&&check.collision); /* undo snapshot */
    assert(EditorMap_DuplicatePoint(&map,index,offset,&duplicate));
    assert(EditorMap_GetModel(&map,duplicate,&check)&&check.origin[0]==56&&check.origin[2]==112);
    assert(check.modelscale==1.25&&check.skin==2&&!check.collision&&!strcmp(check.model,model.model));
    for(int a=0;a<3;++a)assert(check.angles[a]==model.angles[a]);
    char *text=NULL,error[128];size_t length;
    assert(EditorMap_Serialize(&map,&text,&length)&&EditorMap_Parse(&loaded,text,length,error,sizeof(error)));free(text);
    assert(EditorMap_GetModel(&loaded,duplicate,&check)&&check.skin==2&&check.angles[1]==90);
    editor_map_model_t transform=model;transform.origin[0]=10;transform.origin[1]=20;transform.origin[2]=30;
    transform.angles[0]=transform.angles[2]=0;transform.angles[1]=90;transform.modelscale=2;
    const double mins[3]={0,0,0},maxs[3]={1,2,3};double corners[8][3];
    assert(EditorMap_ModelCorners(&transform,mins,maxs,corners));
    assert(fabs(corners[7][0]-6)<1e-9&&fabs(corners[7][1]-22)<1e-9&&fabs(corners[7][2]-36)<1e-9);
    transform.angles[0]=90;transform.angles[1]=0;
    assert(EditorMap_ModelCorners(&transform,mins,maxs,corners));
    assert(fabs(corners[1][0]-10)<1e-9&&fabs(corners[1][2]-32)<1e-9);
    /* Document parsing accepts a static-model reference without loading its
       asset. Invalid edit values cannot partially mutate prior properties. */
    model.modelscale=0;assert(!EditorMap_SetModel(&map,index,&model));
    model.modelscale=1e-300;assert(!EditorMap_SetModel(&map,index,&model));
    model.modelscale=INFINITY;assert(!EditorMap_SetModel(&map,index,&model));
    model.modelscale=1.25;model.skin=-1;assert(!EditorMap_SetModel(&map,index,&model));model.skin=2;
    model.origin[2]=NAN;assert(!EditorMap_SetModel(&map,index,&model));model.origin[2]=96;
    assert(EditorMap_GetModel(&map,index,&check)&&check.modelscale==1.25&&check.skin==2&&check.origin[2]==96);
    assert(EditorMap_SetProperty(&loaded.entities[duplicate],"collision","maybe"));assert(!EditorMap_GetModel(&loaded,duplicate,&check));
    assert(EditorMap_RemoveEntity(&map,duplicate)&&map.num_entities==saved.num_entities);
    /* Optional fields use schema defaults for hand-authored MAP entities. */
    assert(EditorMap_AddEntity(&map,"quiver_static_model",&duplicate));
    assert(EditorMap_SetProperty(&map.entities[duplicate],"model","progs/animated.md5mesh"));
    assert(EditorMap_GetModel(&map,duplicate,&check)&&check.modelscale==1&&check.skin==0&&check.collision);
    EditorMap_Free(&map);EditorMap_Free(&saved);EditorMap_Free(&loaded);
}

int main(void)
{
    point_entities();
    light_colors();
    model_entities();
    editor_map_t map = {0}, copy = {0};
    const double lo[3] = {-64,-128,-32}, hi[3] = {64,128,96};
    assert(EditorMap_Init(&map));
    assert(EditorMap_AddBox(&map, 0, lo, hi, "stone", NULL));
    editor_map_brush_t *b = &map.entities[0].brushes[0];
    bounds(b, lo, hi);
    for (int i = 0; i < 6; ++i) {
        const double (*p)[3] = b->faces[i].points;
        double u[3], v[3], n[3], center[3];
        for (int j = 0; j < 3; ++j) { u[j] = p[1][j]-p[0][j]; v[j] = p[2][j]-p[0][j]; center[j] = (lo[j]+hi[j])/2-p[0][j]; }
        n[0] = u[1]*v[2]-u[2]*v[1]; n[1] = u[2]*v[0]-u[0]*v[2]; n[2] = u[0]*v[1]-u[1]*v[0];
        assert(n[0]*center[0]+n[1]*center[1]+n[2]*center[2] > 0);
		double vertices[64][3];
		assert(EditorMap_BrushFaceVertices(b, i, vertices, 64) == 4);
		assert(EditorMap_BrushFaceVertices(b, i, vertices, 3) == 0);
		for (int vtx = 0; vtx < 4; ++vtx) {
			for (int j = 0; j < 3; ++j) assert(vertices[vtx][j] >= lo[j] - 1e-7 && vertices[vtx][j] <= hi[j] + 1e-7);
			assert(fabs(n[0]*(vertices[vtx][0]-p[0][0]) + n[1]*(vertices[vtx][1]-p[0][1]) + n[2]*(vertices[vtx][2]-p[0][2])) < 1e-7);
		}
    }
    assert(EditorMap_SetProperty(&map.entities[0], "message", "a \\\"quote\\\" \\\\ path"));
    size_t spawn;
    assert(EditorMap_AddEntity(&map, "info_player_start", &spawn));
    assert(EditorMap_SetProperty(&map.entities[spawn], "origin", "0 0 32"));
    assert(!EditorMap_RemoveEntity(&map, 0));
    assert(!EditorMap_SetProperty(&map.entities[0], "invalid", "line\nbreak"));
    b = &map.entities[0].brushes[0];
    const char *valve = "stone [ 1 0 0 16 ] [ 0 -1 0 8 ] 0 0.5 0.5 123 4 5 // note ( }";
    free(b->faces[0].mapping);
    b->faces[0].mapping = malloc(strlen(valve)+1);
    assert(b->faces[0].mapping);
    strcpy(b->faces[0].mapping, valve);
    char *encoded = NULL, error[256]; size_t length;
    assert(EditorMap_Serialize(&map, &encoded, &length));
    assert(EditorMap_Parse(&copy, encoded, length, error, sizeof(error)));
    assert(strcmp(copy.entities[0].brushes[0].faces[0].mapping, valve) == 0);
    assert(strcmp(EditorMap_GetProperty(&copy.entities[0], "message"), EditorMap_GetProperty(&map.entities[0], "message")) == 0);
    bounds(&copy.entities[0].brushes[0], lo, hi);
    free(encoded);
    assert(EditorMap_Clone(&copy, &map));
    b = &copy.entities[0].brushes[0];
    double delta[3] = {10,-5,3}, moved_lo[3] = {-54,-133,-29}, moved_hi[3] = {74,123,99};
    assert(EditorMap_BrushTranslate(b, delta)); bounds(b, moved_lo, moved_hi);
    assert(EditorMap_BrushResizeBox(b, lo, hi)); bounds(b, lo, hi);
    assert(strcmp(b->faces[0].mapping, valve) == 0);
    const char *bad[] = {
        "{ \"classname\" \"worldspawn\" } /*",
        "{ \"classname\" \"worldspawn\" { ( 0 0 0 ) ( 1 0 0 ) ( 0 1 0 ) stone 0 } }",
        "{ \"classname\" \"worldspawn\"",
        "{ \"classname\" \"worldspawn\" { ( nan 0 0 ) ( 1 0 0 ) ( 0 1 0 ) stone 0 0 0 1 1 } }",
        "{ \"classname\" \"not_worldspawn\" }"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        assert(!EditorMap_Parse(&copy, bad[i], strlen(bad[i]), error, sizeof(error)));
        assert(error[0]);
        assert(copy.entities[0].num_brushes == 1);
        bounds(&copy.entities[0].brushes[0], lo, hi);
    }
    const char *tetra = "{\n\"classname\" \"worldspawn\"\n{\n"
        "(0 0 0) (0 1 0) (0 0 1) t 0 0 0 1 1\n"
        "(0 0 0) (0 0 1) (1 0 0) t 0 0 0 1 1\n"
        "(0 0 0) (1 0 0) (0 1 0) t 0 0 0 1 1\n"
        "(1 0 0) (0 0 1) (0 1 0) t 0 0 0 1 1\n} }\n";
    assert(EditorMap_Parse(&copy, tetra, strlen(tetra), error, sizeof(error)));
    double tlo[3] = {0,0,0}, thi[3] = {1,1,1};
    bounds(&copy.entities[0].brushes[0], tlo, thi);
	for (int face = 0; face < 4; ++face) {
		double vertices[64][3];
		assert(EditorMap_BrushFaceVertices(&copy.entities[0].brushes[0], face, vertices, 64) == 3);
	}
    assert(!EditorMap_BrushResizeBox(&copy.entities[0].brushes[0], lo, hi));
    assert(EditorMap_RemoveBrush(&map.entities[0], 0));
    assert(EditorMap_RemoveEntity(&map, spawn));
    EditorMap_Free(&copy);
    EditorMap_Free(&map);
    puts("editor_map_test: OK");
    return 0;
}
